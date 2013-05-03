/* Copyright (c) 2009-2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */
/*
 * Qualcomm Tavarua FM core driver
 */

/* driver definitions */
#define DRIVER_AUTHOR "Qualcomm"
#define DRIVER_NAME "radio-tavarua"
#define DRIVER_CARD "Qualcomm FM Radio Transceiver"
#define DRIVER_DESC "I2C radio driver for Qualcomm FM Radio Transceiver "
#define DRIVER_VERSION "1.0.0"

#include <linux/version.h>
#include <linux/init.h>         /* Initdata                     */
#include <linux/delay.h>        /* udelay                       */
#include <linux/uaccess.h>      /* copy to/from user            */
#include <linux/kfifo.h>        /* lock free circular buffer    */
#include <linux/param.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/interrupt.h>

/* kernel includes */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/videodev2.h>
#include <linux/mutex.h>
#include <media/v4l2-common.h>
#include <media/rds.h>
#include <asm/unaligned.h>
#include <media/v4l2-ioctl.h>
#include <linux/unistd.h>
#include <asm/atomic.h>
#include <media/tavarua.h>
#include <linux/mfd/marimba.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/gps_fm_lna.h>

#define TAVARUA_DEFAULT_TH  -83

/*
regional parameters for radio device
*/
struct region_params_t {
	enum tavarua_region_t region;
	unsigned int band_high;
	unsigned int band_low;
	char emphasis;
	char rds_std;
	char spacing;
};

struct srch_params_t {
	unsigned short srch_pi;
	unsigned char srch_pty;
	unsigned int preset_num;
	int get_list;
};

/* Main radio device structure,
acts as a shadow copy of the
actual tavaura registers */
struct tavarua_device {
	struct video_device *videodev;
	/* driver management */
	int users;
	/* top level driver data */
	struct marimba *marimba;
	struct device *dev;
	/* platform specific functionality */
	struct marimba_fm_platform_data *pdata;
	unsigned int chipID;
	/*RDS buffers + Radio event buffer*/
	struct kfifo data_buf[TAVARUA_BUF_MAX];
	/* search paramters */
	struct srch_params_t srch_params;
	/* keep track of pending xfrs */
	int pending_xfrs[TAVARUA_XFR_MAX];
	int xfr_bytes_left;
	int xfr_in_progress;
	/* Transmit data */
	enum tavarua_xfr_ctrl_t tx_mode;
	/* synchrnous xfr data */
	unsigned char sync_xfr_regs[XFR_REG_NUM];
	struct completion sync_xfr_start;
	struct completion sync_req_done;
	int tune_req;
	/* internal register status */
	unsigned char registers[RADIO_REGISTERS];
	/* regional settings */
	struct region_params_t region_params;
	/* power mode */
	int lp_mode;
	int handle_irq;
	/* global lock */
	struct mutex lock;
	/* buffer locks*/
	spinlock_t buf_lock[TAVARUA_BUF_MAX];
	/* work queue */
	struct workqueue_struct *wqueue;
	struct delayed_work work;
	/* wait queue for blocking event read */
	wait_queue_head_t event_queue;
	/* wait queue for raw rds read */
	wait_queue_head_t read_queue;
};

/**************************************************************************
 * Module Parameters
 **************************************************************************/

/* Radio Nr */
static int radio_nr = -1;
module_param(radio_nr, int, 0);
MODULE_PARM_DESC(radio_nr, "Radio Nr");
static int wait_timeout = WAIT_TIMEOUT;
/* Bahama's version*/
static u8 bahama_version;
/* RDS buffer blocks */
static unsigned int rds_buf = 100;
module_param(rds_buf, uint, 0);
MODULE_PARM_DESC(rds_buf, "RDS buffer entries: *100*");
/* static variables */
static struct tavarua_device *private_data;
/* forward declerations */
static int tavarua_disable_interrupts(struct tavarua_device *radio);
static int tavarua_setup_interrupts(struct tavarua_device *radio,
					enum radio_state_t state);
static int tavarua_start(struct tavarua_device *radio,
			enum radio_state_t state);
static int tavarua_request_irq(struct tavarua_device *radio);
static void start_pending_xfr(struct tavarua_device *radio);
/* work function */
static void read_int_stat(struct work_struct *work);

#define GPS_FM_LNA_2V8_GPIO   79

int enable_gps_fm_lna(int bEnable)
{
    static int      gps_fm_lna_refcnt = 0;
    uint32_t        irqcfg;
    int             rc;

    if (bEnable)
    {
        if (gps_fm_lna_refcnt==0)
        {
            irqcfg = GPIO_CFG(GPS_FM_LNA_2V8_GPIO, 0, GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL,GPIO_CFG_2MA);
            rc = gpio_tlmm_config(irqcfg, GPIO_CFG_ENABLE);
            if (rc) 
            {
                printk(KERN_ERR "%s: gpio_tlmm_config(%#x)=%d\n", __func__, irqcfg, rc);
                rc = -EIO;
                return  -1;
            }
            gpio_set_value(GPS_FM_LNA_2V8_GPIO,1);
        }

        if (gps_fm_lna_refcnt<INT_MAX)
        {
            gps_fm_lna_refcnt++;  
        }
    }
    else
    {
        if (!gps_fm_lna_refcnt)
            return 0;

        if (gps_fm_lna_refcnt==1)
            gpio_set_value(GPS_FM_LNA_2V8_GPIO,0);

        gps_fm_lna_refcnt--;
    }
    return gps_fm_lna_refcnt;
}

static int is_bahama(void)
{
	int id = 0;

	switch (id = adie_get_detected_connectivity_type()) {
	case BAHAMA_ID:
		FMDBG("It is Bahama\n");
		return 1;

	case MARIMBA_ID:
		FMDBG("It is Marimba\n");
		return 0;
	default:
		printk(KERN_ERR "%s: unexpected adie connectivity type: %d\n",
			__func__, id);
		return -ENODEV;
	}
}

static int set_fm_slave_id(struct tavarua_device *radio)
{
	int bahama_present = is_bahama();

	if (bahama_present == -ENODEV)
		return -ENODEV;

	if (bahama_present)
		radio->marimba->mod_id = SLAVE_ID_BAHAMA_FM;
	else
		radio->marimba->mod_id = MARIMBA_SLAVE_ID_FM;

	return 0;
}

/*=============================================================================
FUNCTION:  tavarua_isr
=============================================================================*/
/**
  This function is called when GPIO is toggled. This functions queues the event
  to interrupt queue, which is later handled by isr handling funcion.
  i.e. INIT_DELAYED_WORK(&radio->work, read_int_stat);

  @param irq: irq that is toggled.
  @param dev_id: structure pointer passed by client.

  @return IRQ_HANDLED.
*/
static irqreturn_t tavarua_isr(int irq, void *dev_id)
{
	struct tavarua_device *radio = dev_id;
	/* schedule a tasklet to handle host intr */
  /* The call to queue_delayed_work ensures that a minimum delay (in jiffies)
   * passes before the work is actually executed. The return value from the
   * function is nonzero if the work_struct was actually added to queue
   * (otherwise, it may have already been there and will not be added a second
   * time).
   */
	queue_delayed_work(radio->wqueue, &radio->work,
				msecs_to_jiffies(TAVARUA_DELAY));
	return IRQ_HANDLED;
}

/**************************************************************************
 * Interface to radio internal registers over top level marimba driver
 *************************************************************************/

/*=============================================================================
FUNCTION:  tavarua_read_registers
=============================================================================*/
/**
  This function is called to read a number of bytes from an I2C interface.
  The bytes read are stored in internal register status (shadow copy).

  @param radio: structure pointer passed by client.
  @param offset: register offset.
  @param len: num of bytes.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int tavarua_read_registers(struct tavarua_device *radio,
				unsigned char offset, int len)
{
	int retval = 0, i = 0;
	retval = set_fm_slave_id(radio);

	if (retval == -ENODEV)
		return retval;

	FMDBG_I2C("I2C Slave: %x, Read Offset(%x): Data [",
						radio->marimba->mod_id,
						offset);

	retval =  marimba_read(radio->marimba, offset,
				&radio->registers[offset], len);

	if (retval > 0) {
		for (i = 0; i < len; i++)
			FMDBG_I2C("%02x ", radio->registers[offset+i]);
		FMDBG_I2C(" ]\n");

	}
	return retval;
}

/*=============================================================================
FUNCTION:  tavarua_write_register
=============================================================================*/
/**
  This function is called to write a byte over the I2C interface.
  The corresponding shadow copy is stored in internal register status.

  @param radio: structure pointer passed by client.
  @param offset: register offset.
  @param value: buffer to be written to the registers.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int tavarua_write_register(struct tavarua_device *radio,
			unsigned char offset, unsigned char value)
{
	int retval;
	retval = set_fm_slave_id(radio);

	if (retval == -ENODEV)
		return retval;

	FMDBG_I2C("I2C Slave: %x, Write Offset(%x): Data[",
						radio->marimba->mod_id,
						offset);
	retval = marimba_write(radio->marimba, offset, &value, 1);
	if (retval > 0) {
		if (offset < RADIO_REGISTERS) {
			radio->registers[offset] = value;
			FMDBG_I2C("%02x ", radio->registers[offset]);
		}
		FMDBG_I2C(" ]\n");
	}
	return retval;
}

/*=============================================================================
FUNCTION:  tavarua_write_registers
=============================================================================*/
/**
  This function is called to write a number of bytes over the I2C interface.
  The corresponding shadow copy is stored in internal register status.

  @param radio: structure pointer passed by client.
  @param offset: register offset.
  @param buf: buffer to be written to the registers.
  @param len: num of bytes.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int tavarua_write_registers(struct tavarua_device *radio,
			unsigned char offset, unsigned char *buf, int len)
{

	int i;
	int retval;
	retval = set_fm_slave_id(radio);

	if (retval == -ENODEV)
		return retval;

	FMDBG_I2C("I2C Slave: %x, Write Offset(%x): Data[",
						radio->marimba->mod_id,
						offset);
	retval = marimba_write(radio->marimba, offset, buf, len);
	if (retval > 0) { /* if write successful, update internal state too */
		for (i = 0; i < len; i++) {
			if ((offset+i) < RADIO_REGISTERS) {
				radio->registers[offset+i] = buf[i];
				FMDBG_I2C("%x ",  radio->registers[offset+i]);
			}
		}
		FMDBG_I2C(" ]\n");
	}
	return retval;
}

/*=============================================================================
FUNCTION:  read_data_blocks
=============================================================================*/
/**
  This function reads Raw RDS blocks from Core regs to driver
  internal regs (shadow copy).

  @param radio: structure pointer passed by client.
  @param offset: register offset.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int read_data_blocks(struct tavarua_device *radio, unsigned char offset)
{
	/* read all 3 RDS blocks */
	return tavarua_read_registers(radio, offset, RDS_BLOCK*4);
}

/*=============================================================================
FUNCTION:  tavarua_rds_read
=============================================================================*/
/**
  This is a rds processing function reads that reads Raw RDS blocks from Core
  regs to driver internal regs (shadow copy). It then fills the V4L2 RDS buffer,
  which is read by App using JNI interface.

  @param radio: structure pointer passed by client.

  @return None.
*/
static void tavarua_rds_read(struct tavarua_device *radio)
{
	struct kfifo *rds_buf = &radio->data_buf[TAVARUA_BUF_RAW_RDS];
	unsigned char blocknum;
	unsigned char tmp[3];

	if (read_data_blocks(radio, RAW_RDS) < 0)
		return;
	 /* copy all four RDS blocks to internal buffer */
	for (blocknum = 0; blocknum < RDS_BLOCKS_NUM; blocknum++) {
		/* Fill the V4L2 RDS buffer */
		put_unaligned(cpu_to_le16(radio->registers[RAW_RDS +
			blocknum*RDS_BLOCK]), (unsigned short *) tmp);
		tmp[2] = blocknum;		/* offset name */
		tmp[2] |= blocknum << 3;	/* received offset */
		tmp[2] |= 0x40; /* corrected error(s) */

		/* copy RDS block to internal buffer */
		kfifo_in_locked(rds_buf, tmp, 3, &radio->buf_lock[TAVARUA_BUF_RAW_RDS]);
	}
	/* wake up read queue */
	if (kfifo_len(rds_buf))
		wake_up_interruptible(&radio->read_queue);

}

/*=============================================================================
FUNCTION:  request_read_xfr
=============================================================================*/
/**
  This function sets the desired MODE in the XFRCTRL register and also sets the
  CTRL field to read.
  This is an asynchronous way of reading the XFR registers. Client would request
  by setting the desired mode in the XFRCTRL register and then would initiate
  the actual data register read by calling copy_from_xfr up on SOC signals
  success.

  NOTE:

  The Data Transfer (XFR) registers are used to pass various data and
  configuration parameters between the Core and host processor.

  To read from the XFR registers, the host processor must set the desired MODE
  in the XFRCTRL register and set the CTRL field to read. The Core will then
  populate the XFRDAT0 - XFRDAT15 registers with the defined mode bytes. The
  Core will set the TRANSFER interrupt status bit and interrupt the host if the
  TRANSFERCTRL interrupt control bit is set. The host can then extract the XFR
  mode bytes once it detects that the Core has updated the registers.

  @param radio: structure pointer passed by client.

  @return Always returns 0.
*/
static int request_read_xfr(struct tavarua_device *radio,
				enum tavarua_xfr_ctrl_t mode){

	tavarua_write_register(radio, XFRCTRL, mode);
	msleep(TAVARUA_DELAY);
	return 0;
}

/*=============================================================================
FUNCTION:  copy_from_xfr
=============================================================================*/
/**
  This function is used to read XFR mode bytes once it detects that the Core
  has updated the registers. It also updates XFR regs to the appropriate
  internal buffer n bytes.

  NOTE:

  This function should be used in conjuction with request_read_xfr. Refer
  request_read_xfr for XFR mode transaction details.

  @param radio: structure pointer passed by client.
  @param buf_type: Index into RDS/Radio event buffer to use.
  @param len: num of bytes.

  @return Always returns 0.
*/
static int copy_from_xfr(struct tavarua_device *radio,
		enum tavarua_buf_t buf_type, unsigned int n){

	struct kfifo *data_fifo = &radio->data_buf[buf_type];
	unsigned char *xfr_regs = &radio->registers[XFRCTRL+1];
	kfifo_in_locked(data_fifo, xfr_regs, n, &radio->buf_lock[buf_type]);
	return 0;
}

/*=============================================================================
FUNCTION:  write_to_xfr
=============================================================================*/
/**
  This function sets the desired MODE in the XFRCTRL register and it also sets
  the CTRL field and data to write.
  This also writes all the XFRDATx registers with the desired input buffer.

  NOTE:

  The Data Transfer (XFR) registers are used to pass various data and
  configuration parameters between the Core and host processor.

  To write data to the Core, the host processor updates XFRDAT0 - XFRDAT15 with
  the appropriate mode bytes. The host processor must then set the desired MODE
  in the XFRCTRL register and set the CTRL field to write. The core will detect
  that the XFRCTRL register was written to and will read the XFR mode bytes.
  After reading all the mode bytes, the Core will set the TRANSFER interrupt
  status bit and interrupt the host if the TRANSFERCTRL interrupt control bit
  is set.

  @param radio: structure pointer passed by client.
  @param mode: XFR mode to write in XFRCTRL register.
  @param buf: buffer to be written to the registers.
  @param len: num of bytes.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int write_to_xfr(struct tavarua_device *radio, unsigned char mode,
			char *buf, int len)
{
	char buffer[len+1];
	memcpy(buffer+1, buf, len);
	/* buffer[0] corresponds to XFRCTRL register
	   set the CTRL bit to 1 for write mode
	*/
	buffer[0] = ((1<<7) | mode);
	return tavarua_write_registers(radio, XFRCTRL, buffer, sizeof(buffer));
}

/*=============================================================================
FUNCTION:  xfr_intf_own
=============================================================================*/
/**
  This function is used to check if there is any pending XFR mode operation.
  If yes, wait for it to complete, else update the flag to indicate XFR
  operation is in progress

  @param radio: structure pointer passed by client.

  @return 0      on success.
	-ETIME on timeout.
*/
static int xfr_intf_own(struct tavarua_device *radio)
{

	mutex_lock(&radio->lock);
	if (radio->xfr_in_progress) {
		radio->pending_xfrs[TAVARUA_XFR_SYNC] = 1;
		mutex_unlock(&radio->lock);
		if (!wait_for_completion_timeout(&radio->sync_xfr_start,
			msecs_to_jiffies(wait_timeout)))
			return -ETIME;
	} else {
		FMDBG("gained ownership of xfr\n");
		radio->xfr_in_progress = 1;
		mutex_unlock(&radio->lock);
	}
	return 0;
}

/*=============================================================================
FUNCTION:  sync_read_xfr
=============================================================================*/
/**
  This function is used to do synchronous XFR read operation.

  @param radio: structure pointer passed by client.
  @param xfr_type: XFR mode to write in XFRCTRL register.
  @param buf: buffer to be read from the core.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int sync_read_xfr(struct tavarua_device *radio,
			enum tavarua_xfr_ctrl_t xfr_type, unsigned char *buf)
{
	int retval;
	retval = xfr_intf_own(radio);
	if (retval < 0)
		return retval;
	retval = tavarua_write_register(radio, XFRCTRL, xfr_type);

	if (retval >= 0) {
		/* Wait for interrupt i.e. complete
		(&radio->sync_req_done); call */
		if (!wait_for_completion_timeout(&radio->sync_req_done,
			msecs_to_jiffies(wait_timeout)) || (retval < 0)) {
			retval = -ETIME;
		} else {
			memcpy(buf, radio->sync_xfr_regs, XFR_REG_NUM);
		}
	}
	radio->xfr_in_progress = 0;
	start_pending_xfr(radio);
	FMDBG("%s: %d\n", __func__, retval);
	return retval;
}

/*=============================================================================
FUNCTION:  sync_write_xfr
=============================================================================*/
/**
  This function is used to do synchronous XFR write operation.

  @param radio: structure pointer passed by client.
  @param xfr_type: XFR mode to write in XFRCTRL register.
  @param buf: buffer to be written to the core.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int sync_write_xfr(struct tavarua_device *radio,
		enum tavarua_xfr_ctrl_t xfr_type, unsigned char *buf)
{
	int retval;
	retval = xfr_intf_own(radio);
	if (retval < 0)
		return retval;
	retval = write_to_xfr(radio, xfr_type, buf, XFR_REG_NUM);

	if (retval >= 0) {
		/* Wait for interrupt i.e. complete
		(&radio->sync_req_done); call */
		if (!wait_for_completion_timeout(&radio->sync_req_done,
			msecs_to_jiffies(wait_timeout)) || (retval < 0)) {
			FMDBG("Write xfr timeout");
		}
	}
	radio->xfr_in_progress = 0;
	start_pending_xfr(radio);
	FMDBG("%s: %d\n", __func__,  retval);
	return retval;
}

/*=============================================================================
FUNCTION:  start_pending_xfr
=============================================================================*/
/**
  This function checks if their are any pending xfr interrupts and if
  the interrupts are either RDS PS, RDS RT, RDS AF, SCANNEXT, SEARCH or SYNC
  then initiates corresponding read operation. Preference is given to RAW RDS
  data (SYNC) over processed data (PS, RT, AF, etc) from core.

  @param radio: structure pointer passed by client.

  @return None.
*/
static void start_pending_xfr(struct tavarua_device *radio)
{
	int i;
	enum tavarua_xfr_t xfr;
	for (i = 0; i < TAVARUA_XFR_MAX; i++) {
		if (radio->pending_xfrs[i]) {
			radio->xfr_in_progress = 1;
			xfr = (enum tavarua_xfr_t)i;
			switch (xfr) {
			/* priority given to synchronous xfrs */
			case TAVARUA_XFR_SYNC:
				complete(&radio->sync_xfr_start);
				break;
			/* asynchrnous xfrs */
			case TAVARUA_XFR_SRCH_LIST:
				request_read_xfr(radio, RX_STATIONS_0);
				break;
			case TAVARUA_XFR_RT_RDS:
				request_read_xfr(radio, RDS_RT_0);
				break;
			case TAVARUA_XFR_PS_RDS:
				request_read_xfr(radio, RDS_PS_0);
				break;
			case TAVARUA_XFR_AF_LIST:
				request_read_xfr(radio, RDS_AF_0);
				break;
			default:
				FMDERR("%s: Unsupported XFR %d\n",
					 __func__, xfr);
			}
			radio->pending_xfrs[i] = 0;
			FMDBG("resurrect xfr %d\n", i);
			}
	}
	return;
}

/*=============================================================================
FUNCTION:  tavarua_q_event
=============================================================================*/
/**
  This function is called to queue an event for user.

  NOTE:
  Applications call the VIDIOC_QBUF ioctl to enqueue an empty (capturing) or
  filled (output) buffer in the driver's incoming queue.

  Pleaes refer tavarua_probe where we register different ioctl's for FM.

  @param radio: structure pointer passed by client.
  @param event: event to be queued.

  @return None.
*/
static void tavarua_q_event(struct tavarua_device *radio,
				enum tavarua_evt_t event)
{

	struct kfifo *data_b = &radio->data_buf[TAVARUA_BUF_EVENTS];
	unsigned char evt = event;
	FMDBG("updating event_q with event %x\n", event);
	if (kfifo_in_locked(data_b, &evt, 1, &radio->buf_lock[TAVARUA_BUF_EVENTS]))
		wake_up_interruptible(&radio->event_queue);
}

/*=============================================================================
FUNCTION:  tavarua_start_xfr
=============================================================================*/
/**
  This function is called to process interrupts which require multiple XFR
  operations (RDS search, RDS PS, RDS RT, etc). if any XFR operation is
  already in progress we store information about pending interrupt, which
  will be processed in future when current pending operation is done.

  @param radio: structure pointer passed by client.
  @param pending_id: XFR operation (which requires multiple XFR operations in
	steps) to start.
  @param xfr_id: XFR mode to write in XFRCTRL register.

  @return None.
*/
static void tavarua_start_xfr(struct tavarua_device *radio,
		enum tavarua_xfr_t pending_id, enum tavarua_xfr_ctrl_t xfr_id)
{
		if (radio->xfr_in_progress)
			radio->pending_xfrs[pending_id] = 1;
		else {
			radio->xfr_in_progress = 1;
			request_read_xfr(radio, xfr_id);
		}
}

/*=============================================================================
FUNCTION:  tavarua_handle_interrupts
=============================================================================*/
/**
  This function processes the interrupts.

  NOTE:
  tavarua_q_event is used to queue events in App buffer. i.e. App calls the
  VIDIOC_QBUF ioctl to enqueue an empty (capturing) buffer, which is filled
  by tavarua_q_event call.

  Any async event that requires multiple steps, i.e. search, RT, PS, etc is
  handled one at a time. (We preserve other interrupts when processing one).
  Sync interrupts are given priority.

  @param radio: structure pointer passed by client.

  @return None.
*/
static void tavarua_handle_interrupts(struct tavarua_device *radio)
{
	int i;
	int retval;
	enum tavarua_xfr_ctrl_t xfr_status;
	if (!radio->handle_irq) {
		FMDBG("IRQ happend, but I wont handle it\n");
		return;
	}
	mutex_lock(&radio->lock);
	tavarua_read_registers(radio, STATUS_REG1, STATUS_REG_NUM);

	FMDBG("INTSTAT1 <%x>\n", radio->registers[STATUS_REG1]);
	FMDBG("INTSTAT2 <%x>\n", radio->registers[STATUS_REG2]);
	FMDBG("INTSTAT3 <%x>\n", radio->registers[STATUS_REG3]);

	if (radio->registers[STATUS_REG1] & READY) {
		complete(&radio->sync_req_done);
		tavarua_q_event(radio, TAVARUA_EVT_RADIO_READY);
	}

	/* Tune completed */
	if (radio->registers[STATUS_REG1] & TUNE) {
		if (radio->tune_req) {
			complete(&radio->sync_req_done);
			radio->tune_req = 0;
		}
		tavarua_q_event(radio, TAVARUA_EVT_TUNE_SUCC);
		if (radio->srch_params.get_list) {
			tavarua_start_xfr(radio, TAVARUA_XFR_SRCH_LIST,
							RX_STATIONS_0);
		}
		radio->srch_params.get_list = 0;
		radio->xfr_in_progress = 0;
		radio->xfr_bytes_left = 0;
		for (i = 0; i < TAVARUA_BUF_MAX; i++) {
			if (i >= TAVARUA_BUF_RT_RDS)
				kfifo_reset(&radio->data_buf[i]);
		}
		for (i = 0; i < TAVARUA_XFR_MAX; i++) {
			if (i >= TAVARUA_XFR_RT_RDS)
				radio->pending_xfrs[i] = 0;
		}
		retval = tavarua_read_registers(radio, TUNECTRL, 1);
		/* send to user station parameters */
		if (retval > -1) {
			/* Signal strength */
			if (!(radio->registers[TUNECTRL] & SIGSTATE))
				tavarua_q_event(radio, TAVARUA_EVT_BELOW_TH);
			else
				tavarua_q_event(radio, TAVARUA_EVT_ABOVE_TH);
			/* mono/stereo */
			if ((radio->registers[TUNECTRL] & MOSTSTATE))
				tavarua_q_event(radio, TAVARUA_EVT_STEREO);
			else
				tavarua_q_event(radio, TAVARUA_EVT_MONO);
			/* is RDS available */
			if ((radio->registers[TUNECTRL] & RDSSYNC))
				tavarua_q_event(radio, TAVARUA_EVT_RDS_AVAIL);
			else
				tavarua_q_event(radio,
						TAVARUA_EVT_RDS_NOT_AVAIL);
		}

	} else {
		if (radio->tune_req) {
			FMDERR("Tune INT is pending\n");
			mutex_unlock(&radio->lock);
			return;
		}
	}
	/* Search completed (read FREQ) */
	if (radio->registers[STATUS_REG1] & SEARCH)
		tavarua_q_event(radio, TAVARUA_EVT_SEEK_COMPLETE);

	/* Scanning for next station */
	if (radio->registers[STATUS_REG1] & SCANNEXT)
		tavarua_q_event(radio, TAVARUA_EVT_SCAN_NEXT);

	/* Signal indicator change (read SIGSTATE) */
	if (radio->registers[STATUS_REG1] & SIGNAL) {
		retval = tavarua_read_registers(radio, TUNECTRL, 1);
		if (retval > -1) {
			if (!(radio->registers[TUNECTRL] & SIGSTATE))
				tavarua_q_event(radio, TAVARUA_EVT_BELOW_TH);
			else
				tavarua_q_event(radio, TAVARUA_EVT_ABOVE_TH);
		}
	}

	/* RDS synchronization state change (read RDSSYNC) */
	if (radio->registers[STATUS_REG1] & SYNC) {
		retval = tavarua_read_registers(radio, TUNECTRL, 1);
		if (retval > -1) {
			if ((radio->registers[TUNECTRL] & RDSSYNC))
				tavarua_q_event(radio, TAVARUA_EVT_RDS_AVAIL);
			else
				tavarua_q_event(radio,
						TAVARUA_EVT_RDS_NOT_AVAIL);
		}
	}

	/* Audio Control indicator (read AUDIOIND) */
	if (radio->registers[STATUS_REG1] & AUDIO) {
		retval = tavarua_read_registers(radio, AUDIOIND, 1);
		if (retval > -1) {
			if ((radio->registers[AUDIOIND] & 0x01))
				tavarua_q_event(radio, TAVARUA_EVT_STEREO);
			else
				tavarua_q_event(radio, TAVARUA_EVT_MONO);
		}
	}

	/* interrupt register 2 */

	/* New unread RDS data group available */
	if (radio->registers[STATUS_REG2] & RDSDAT) {
		FMDBG("Raw RDS Available\n");
		tavarua_rds_read(radio);
		tavarua_q_event(radio, TAVARUA_EVT_NEW_RAW_RDS);
	}

	/* New RDS Program Service Table available */
	if (radio->registers[STATUS_REG2] & RDSPS) {
		FMDBG("New PS RDS\n");
		tavarua_start_xfr(radio, TAVARUA_XFR_PS_RDS, RDS_PS_0);
	}

	/* New RDS Radio Text available */
	if (radio->registers[STATUS_REG2] & RDSRT) {
		FMDBG("New RT RDS\n");
		tavarua_start_xfr(radio, TAVARUA_XFR_RT_RDS, RDS_RT_0);
	}

	/* New RDS Radio Text available */
	if (radio->registers[STATUS_REG2] & RDSAF) {
		FMDBG("New AF RDS\n");
		tavarua_start_xfr(radio, TAVARUA_XFR_AF_LIST, RDS_AF_0);
	}

	/* interrupt register 3 */

	/* Data transfer (XFR) completed */
	if (radio->registers[STATUS_REG3] & TRANSFER) {
		FMDBG("XFR Interrupt\n");
		tavarua_read_registers(radio, XFRCTRL, XFR_REG_NUM+1);
		FMDBG("XFRCTRL IS: %x\n", radio->registers[XFRCTRL]);
		xfr_status = (enum tavarua_xfr_ctrl_t)radio->registers[XFRCTRL];
		switch (xfr_status) {
		case RDS_PS_0:
			FMDBG("PS Header\n");
			copy_from_xfr(radio, TAVARUA_BUF_PS_RDS, 5);
			radio->xfr_bytes_left = (radio->registers[XFRCTRL+1] &
								0x0F) * 8;
			FMDBG("PS RDS Length: %d\n", radio->xfr_bytes_left);
			if ((radio->xfr_bytes_left > 0) &&
			    (radio->xfr_bytes_left < 97))
				request_read_xfr(radio,	RDS_PS_1);
			else
				radio->xfr_in_progress = 0;
			break;
		case RDS_PS_1:
		case RDS_PS_2:
		case RDS_PS_3:
		case RDS_PS_4:
		case RDS_PS_5:
		case RDS_PS_6:
			FMDBG("PS Data\n");
			copy_from_xfr(radio, TAVARUA_BUF_PS_RDS, XFR_REG_NUM);
			radio->xfr_bytes_left -= XFR_REG_NUM;
			if (radio->xfr_bytes_left > 0) {
				if ((xfr_status + 1) > RDS_PS_6)
					request_read_xfr(radio,	RDS_PS_6);
				else
					request_read_xfr(radio,	xfr_status+1);
			} else {
				radio->xfr_in_progress = 0;
				tavarua_q_event(radio, TAVARUA_EVT_NEW_PS_RDS);
			}
			break;
		case RDS_RT_0:
			FMDBG("RT Header\n");
			copy_from_xfr(radio, TAVARUA_BUF_RT_RDS, 5);
			radio->xfr_bytes_left = radio->registers[XFRCTRL+1]
									& 0x7F;
			FMDBG("RT RDS Length: %d\n", radio->xfr_bytes_left);
			if (radio->xfr_bytes_left > 0)
				request_read_xfr(radio, RDS_RT_1);
			break;
		case RDS_RT_1:
		case RDS_RT_2:
		case RDS_RT_3:
		case RDS_RT_4:
			FMDBG("xfr interrupt RT data\n");
			copy_from_xfr(radio, TAVARUA_BUF_RT_RDS, XFR_REG_NUM);
			radio->xfr_bytes_left -= XFR_REG_NUM;
			if (radio->xfr_bytes_left > 0) {
				request_read_xfr(radio,	xfr_status+1);
			} else {
				radio->xfr_in_progress = 0;
				tavarua_q_event(radio, TAVARUA_EVT_NEW_RT_RDS);
			}
			break;
		case RDS_AF_0:
			copy_from_xfr(radio, TAVARUA_BUF_AF_LIST,
						XFR_REG_NUM);
			radio->xfr_bytes_left = radio->registers[XFRCTRL+5]-11;
			if (radio->xfr_bytes_left > 0)
				request_read_xfr(radio,	RDS_AF_1);
			else
				radio->xfr_in_progress = 0;
			break;
		case RDS_AF_1:
			copy_from_xfr(radio, TAVARUA_BUF_AF_LIST,
						radio->xfr_bytes_left);
			tavarua_q_event(radio, TAVARUA_EVT_NEW_AF_LIST);
			radio->xfr_in_progress = 0;
			break;
		case RX_CONFIG:
		case RADIO_CONFIG:
		case RDS_CONFIG:
			memcpy(radio->sync_xfr_regs,
				&radio->registers[XFRCTRL+1], XFR_REG_NUM);
			complete(&radio->sync_req_done);
			break;
		case RX_STATIONS_0:
			FMDBG("Search list has %d stations\n",
						radio->registers[XFRCTRL+1]);
			radio->xfr_bytes_left = radio->registers[XFRCTRL+1]*2;
			if (radio->xfr_bytes_left > 14) {
				copy_from_xfr(radio, TAVARUA_BUF_SRCH_LIST,
							XFR_REG_NUM);
				request_read_xfr(radio,	RX_STATIONS_1);
			} else if (radio->xfr_bytes_left) {
				FMDBG("In else RX_STATIONS_0\n");
				copy_from_xfr(radio, TAVARUA_BUF_SRCH_LIST,
						radio->xfr_bytes_left+1);
				tavarua_q_event(radio,
						TAVARUA_EVT_NEW_SRCH_LIST);
				radio->xfr_in_progress = 0;
			}
			break;
		case RX_STATIONS_1:
			FMDBG("In RX_STATIONS_1");
			copy_from_xfr(radio, TAVARUA_BUF_SRCH_LIST,
						radio->xfr_bytes_left);
			tavarua_q_event(radio, TAVARUA_EVT_NEW_SRCH_LIST);
			radio->xfr_in_progress = 0;
			break;
		case (0x80 | RX_CONFIG):
		case (0x80 | RADIO_CONFIG):
		case (0x80 | RDS_CONFIG):
		case (0x80 | INT_CTRL):
			complete(&radio->sync_req_done);
			break;
		default:
			FMDERR("UNKNOWN XFR = %d\n", xfr_status);
		}
		if (!radio->xfr_in_progress)
			start_pending_xfr(radio);

	}

	/* Error occurred. Read ERRCODE to determine cause */
	if (radio->registers[STATUS_REG3] & ERROR)
		FMDERR("ERROR STATE\n");

	mutex_unlock(&radio->lock);
	FMDBG("Work is done\n");

}

/*=============================================================================
FUNCTION:  read_int_stat
=============================================================================*/
/**
  This function is scheduled whenever there is an interrupt pending in interrupt
  queue. i.e. kfmradio.

  Whenever there is a GPIO interrupt, a delayed work will be queued in to the
  'kfmradio' work queue. Upon execution of this work in the queue, a  a call
  to read_int_stat function will be made , which would in turn handle the
  interrupts by reading the INTSTATx registers.
  NOTE:
  Tasks to be run out of a workqueue need to be packaged in a struct
  work_struct structure.

  @param work: work_struct structure.

  @return None.
*/
static void read_int_stat(struct work_struct *work)
{
	struct tavarua_device *radio = container_of(work,
					struct tavarua_device, work.work);
	tavarua_handle_interrupts(radio);
}

/*************************************************************************
 * irq helper functions
 ************************************************************************/

/*=============================================================================
FUNCTION:  tavarua_request_irq
=============================================================================*/
/**
  This function is called to acquire a FM GPIO and enable FM interrupts.

  @param radio: structure pointer passed by client.

  @return 0 if success else otherwise.
*/
static int tavarua_request_irq(struct tavarua_device *radio)
{
	int retval;
	int irq = radio->pdata->irq;
	if (radio == NULL)
		return -EINVAL;

  /* A workqueue created with create_workqueue() will have one worker thread
   * for each CPU on the system; create_singlethread_workqueue(), instead,
   * creates a workqueue with a single worker process. The name of the queue
   * is limited to ten characters; it is only used for generating the "command"
   * for the kernel thread(s) (which can be seen in ps or top).
   */
	radio->wqueue  = create_singlethread_workqueue("kfmradio");
	if (!radio->wqueue)
		return -ENOMEM;
  /* allocate an interrupt line */
  /* On success, request_irq() returns 0 if everything goes  as
     planned.  Your interrupt handler will start receiving its
     interrupts immediately. On failure, request_irq()
     returns:
	-EINVAL
		The  IRQ  number  you  requested  was either
		invalid or reserved, or your passed  a  NULL
		pointer for the handler() parameter.

	-EBUSY The  IRQ you requested is already being
		handled, and the IRQ cannot  be  shared.

	-ENXIO The m68k returns this value for  an  invalid
		IRQ number.
  */
	/* Use request_any_context_irq, So that it might work for nested or
	nested interrupts. in MSM8x60, FM is connected to PMIC GPIO and it
	is a nested interrupt*/
	retval = request_any_context_irq(irq, tavarua_isr,
				IRQ_TYPE_EDGE_FALLING, "fm interrupt", radio);
	if (retval < 0) {
		FMDERR("Couldn't acquire FM gpio %d\n", irq);
		return retval;
	} else {
		FMDBG("FM GPIO %d registered\n", irq);
	}
	retval = enable_irq_wake(irq);
	if (retval < 0) {
		FMDERR("Could not enable FM interrupt\n ");
		free_irq(irq , radio);
	}
	return retval;
}

/*=============================================================================
FUNCTION:  tavarua_disable_irq
=============================================================================*/
/**
  This function is called to disable FM irq and free up FM interrupt handling
  resources.

  @param radio: structure pointer passed by client.

  @return 0 if success else otherwise.
*/
static int tavarua_disable_irq(struct tavarua_device *radio)
{
	int irq;
	if (!radio)
		return -EINVAL;
	irq = radio->pdata->irq;
	disable_irq_wake(irq);
	cancel_delayed_work_sync(&radio->work);
	flush_workqueue(radio->wqueue);
	free_irq(irq, radio);
	destroy_workqueue(radio->wqueue);
	return 0;
}

/*************************************************************************
 * fops/IOCTL helper functions
 ************************************************************************/

/*=============================================================================
FUNCTION:  tavarua_search
=============================================================================*/
/**
  This interface sets the search control features.

  @param radio: structure pointer passed by client.
  @param on: The value of a control.
  @param dir: FM search direction.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int tavarua_search(struct tavarua_device *radio, int on, int dir)
{
	enum search_t srch = radio->registers[SRCHCTRL] & SRCH_MODE;

	FMDBG("In tavarua_search\n");
	if (on) {
		radio->registers[SRCHRDS1] = 0x00;
		radio->registers[SRCHRDS2] = 0x00;
		/* Set freq band */
		switch (srch) {
		case SCAN_FOR_STRONG:
		case SCAN_FOR_WEAK:
			radio->srch_params.get_list = 1;
			radio->registers[SRCHRDS2] =
					radio->srch_params.preset_num;
			break;
		case RDS_SEEK_PTY:
		case RDS_SCAN_PTY:
			radio->registers[SRCHRDS2] =
					radio->srch_params.srch_pty;
			break;
		case RDS_SEEK_PI:
			radio->registers[SRCHRDS1] =
				(radio->srch_params.srch_pi & 0xFF00) >> 8;
			radio->registers[SRCHRDS2] =
				(radio->srch_params.srch_pi & 0x00FF);
			break;
		default:
			break;
		}
		radio->registers[SRCHCTRL] |= SRCH_ON;
	} else {
		radio->registers[SRCHCTRL] &= ~SRCH_ON;
		radio->srch_params.get_list = 0;
	}
	radio->registers[SRCHCTRL] = (dir << 3) |
				(radio->registers[SRCHCTRL] & 0xF7);

	FMDBG("SRCHCTRL <%x>\n", radio->registers[SRCHCTRL]);
	FMDBG("Search Started\n");
	return tavarua_write_registers(radio, SRCHRDS1,
				&radio->registers[SRCHRDS1], 3);
}

/*=============================================================================
FUNCTION:  tavarua_set_region
=============================================================================*/
/**
  This interface configures the FM radio.

  @param radio: structure pointer passed by client.
  @param req_region: FM band types.  These types defines the FM band minimum and
  maximum frequencies in the FM band.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int tavarua_set_region(struct tavarua_device *radio,
				int req_region)
{
	int retval = 0;
	unsigned char xfr_buf[XFR_REG_NUM];
	unsigned char value;
	unsigned int spacing = 0.100 * FREQ_MUL;
	unsigned int band_low, band_high;
	unsigned int low_band_limit = 76.0 * FREQ_MUL;
	enum tavarua_region_t region = req_region;

	/* Set freq band */
	switch (region) {
	case TAVARUA_REGION_US:
	case TAVARUA_REGION_EU:
		SET_REG_FIELD(radio->registers[RDCTRL], 0,
			RDCTRL_BAND_OFFSET, RDCTRL_BAND_MASK);
		break;
	case TAVARUA_REGION_JAPAN:
		SET_REG_FIELD(radio->registers[RDCTRL], 1,
			RDCTRL_BAND_OFFSET, RDCTRL_BAND_MASK);
		break;
	default:
		retval = sync_read_xfr(radio, RADIO_CONFIG, xfr_buf);
		if (retval < 0) {
			FMDERR("failed to get RADIO_CONFIG\n");
			return retval;
		}
		band_low = (radio->region_params.band_low -
					low_band_limit) / spacing;
		band_high = (radio->region_params.band_high -
					low_band_limit) / spacing;
		FMDBG("low_band: %x, high_band: %x\n", band_low, band_high);
		xfr_buf[0] = band_low >> 8;
		xfr_buf[1] = band_low & 0xFF;
		xfr_buf[2] = band_high >> 8;
		xfr_buf[3] = band_high & 0xFF;
		retval = sync_write_xfr(radio, RADIO_CONFIG, xfr_buf);
		if (retval < 0) {
			FMDERR("Could not set regional settings\n");
			return retval;
		}
		break;
	}

	/* Set channel spacing */
	switch (region) {
	case TAVARUA_REGION_US:
	case TAVARUA_REGION_EU:
	case TAVARUA_REGION_JAPAN:
	case TAVARUA_REGION_JAPAN_WIDE:
		value = 0;
		break;
	default:
		value = radio->region_params.spacing;
	}

	SET_REG_FIELD(radio->registers[RDCTRL], value,
		RDCTRL_CHSPACE_OFFSET, RDCTRL_CHSPACE_MASK);

	/* Set De-emphasis and soft band range*/
	switch (region) {
	case TAVARUA_REGION_US:
	case TAVARUA_REGION_JAPAN:
	case TAVARUA_REGION_JAPAN_WIDE:
		value = 0;
		break;
	case TAVARUA_REGION_EU:
		value = 1;
		break;
	default:
		value = radio->region_params.emphasis;
	}

	SET_REG_FIELD(radio->registers[RDCTRL], value,
		RDCTRL_DEEMPHASIS_OFFSET, RDCTRL_DEEMPHASIS_MASK);

	/* set RDS standard */
	switch (region) {
	default:
		value = radio->region_params.rds_std;
		break;
	case TAVARUA_REGION_US:
		value = 0;
		break;
	case TAVARUA_REGION_EU:
		value = 1;
		break;
	}
	SET_REG_FIELD(radio->registers[RDSCTRL], value,
		RDSCTRL_STANDARD_OFFSET, RDSCTRL_STANDARD_MASK);

	FMDBG("RDSCTRLL %x\n", radio->registers[RDSCTRL]);
	retval = tavarua_write_register(radio, RDSCTRL,
					radio->registers[RDSCTRL]);
	if (retval < 0)
		return retval;

	FMDBG("RDCTRL: %x\n", radio->registers[RDCTRL]);
	retval = tavarua_write_register(radio, RDCTRL,
					radio->registers[RDCTRL]);
	if (retval < 0) {
		FMDERR("Could not set region in rdctrl\n");
		return retval;
	}

	/* setting soft band */
	switch (region) {
	case TAVARUA_REGION_US:
	case TAVARUA_REGION_EU:
		radio->region_params.band_low = 87.5 * FREQ_MUL;
		radio->region_params.band_high = 108 * FREQ_MUL;
		break;
	case TAVARUA_REGION_JAPAN:
		radio->region_params.band_low = 76 * FREQ_MUL;
		radio->region_params.band_high = 90 * FREQ_MUL;
		break;
	default:
		break;
	}
	radio->region_params.region = region;
	return retval;
}

/*=============================================================================
FUNCTION:  tavarua_get_freq
=============================================================================*/
/**
  This interface gets the current frequency.

  @param radio: structure pointer passed by client.
  @param freq: struct v4l2_frequency. This will be set to the resultant
  frequency in units of 62.5 kHz on success.

  NOTE:
  To get the current tuner or modulator radio frequency applications set the
  tuner field of a struct v4l2_frequency to the respective tuner or modulator
  number (only input devices have tuners, only output devices have modulators),
  zero out the reserved array and call the VIDIOC_G_FREQUENCY ioctl with a
  pointer to this structure. The driver stores the current frequency in the
  frequency field.

  Tuning frequency is in units of 62.5 kHz, or if the struct v4l2_tuner or
  struct v4l2_modulator capabilities flag V4L2_TUNER_CAP_LOW is set, in
  units of 62.5 Hz.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int tavarua_get_freq(struct tavarua_device *radio,
				struct v4l2_frequency *freq)
{
	int retval;
	unsigned short chan;
	unsigned int band_bottom;
	unsigned int spacing;
	band_bottom = radio->region_params.band_low;
	spacing  = 0.100 * FREQ_MUL;
	/* read channel */
	retval = tavarua_read_registers(radio, FREQ, 2);
	chan = radio->registers[FREQ];

	/* Frequency (MHz) = 100 (kHz) x Channel + Bottom of Band (MHz) */
	freq->frequency = spacing * chan + band_bottom;
	if (radio->registers[TUNECTRL] & ADD_OFFSET)
		freq->frequency += 800;
	return retval;
}

/*=============================================================================
FUNCTION:  tavarua_set_freq
=============================================================================*/
/**
  This interface sets the current frequency.

  @param radio: structure pointer passed by client.
  @param freq: desired frequency sent by the client in 62.5 kHz units.

  NOTE:
  To change the current tuner or modulator radio frequency, applications
  initialize the tuner, type and frequency fields, and the reserved array of a
  struct v4l2_frequency and call the VIDIOC_S_FREQUENCY ioctl with a pointer to
  this structure. When the requested frequency is not possible the driver
  assumes the closest possible value. However VIDIOC_S_FREQUENCY is a
  write-only ioctl, it does not return the actual new frequency.

  Tuning frequency is in units of 62.5 kHz, or if the struct v4l2_tuner
  or struct v4l2_modulator capabilities flag V4L2_TUNER_CAP_LOW is set,
  in units of 62.5 Hz.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int tavarua_set_freq(struct tavarua_device *radio, unsigned int freq)
{

	unsigned int band_bottom;
	unsigned char chan;
	unsigned char cmd[] = {0x00, 0x00};
	unsigned int spacing;
	int retval;
	band_bottom = radio->region_params.band_low;
	spacing  = 0.100 * FREQ_MUL;
	if ((freq % 1600) == 800) {
		cmd[1] = ADD_OFFSET;
		freq -= 800;
	}
	/* Chan = [ Freq (Mhz) - Bottom of Band (MHz) ] / 100 (kHz) */
	chan = (freq - band_bottom) / spacing;

	cmd[0] = chan;
	cmd[1] |= TUNE_STATION;
	radio->tune_req = 1;
	retval = tavarua_write_registers(radio, FREQ, cmd, 2);
	if (retval < 0)
		radio->tune_req = 0;
	return retval;

}

/**************************************************************************
 * File Operations Interface
 *************************************************************************/

/*=============================================================================
FUNCTION:  tavarua_fops_read
=============================================================================*/
/**
  This function is called when a process, which already opened the dev file,
  attempts to read from it.

  In case of tavarua driver, it is called to read RDS data.

  @param file: file descriptor.
	@param buf: The buffer to fill with data.
	@param count: The length of the buffer in bytes.
	@param ppos: Our offset in the file.

  @return The number of bytes put into the buffer on sucess.
	-EFAULT if there is no access to user buffer
*/
static ssize_t tavarua_fops_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct tavarua_device *radio = video_get_drvdata(video_devdata(file));
	struct kfifo *rds_buf = &radio->data_buf[TAVARUA_BUF_RAW_RDS];

	/* block if no new data available */
	while (!kfifo_len(rds_buf)) {
		if (file->f_flags & O_NONBLOCK)
			return -EWOULDBLOCK;
		if (wait_event_interruptible(radio->read_queue,
			kfifo_len(rds_buf)) < 0)
			return -EINTR;
	}

	/* calculate block count from byte count */
	count /= BYTES_PER_BLOCK;


	/* check if we can write to the user buffer */
	if (!access_ok(VERIFY_WRITE, buf, count*BYTES_PER_BLOCK))
		return -EFAULT;

	/* copy RDS block out of internal buffer and to user buffer */
	return kfifo_out_locked(rds_buf, buf, count*BYTES_PER_BLOCK, 
				&radio->buf_lock[TAVARUA_BUF_RAW_RDS]);
}

/*=============================================================================
FUNCTION:  tavarua_fops_write
=============================================================================*/
/**
  This function is called when a process, which already opened the dev file,
  attempts to write to it.

  In case of tavarua driver, it is called to write RDS data to host.

  @param file: file descriptor.
	@param buf: The buffer which has data to write.
	@param count: The length of the buffer.
	@param ppos: Our offset in the file.

  @return The number of bytes written from the buffer.
*/
static ssize_t tavarua_fops_write(struct file *file, const char __user *data,
			size_t count, loff_t *ppos)
{
	struct tavarua_device *radio = video_get_drvdata(video_devdata(file));
	int retval = 0;
	int bytes_to_copy;
	int bytes_copied = 0;
	int bytes_left;
	int chunk_index = 0;
	unsigned char tx_data[XFR_REG_NUM];
	/* Disable TX of this type first */
	switch (radio->tx_mode) {
	case TAVARUA_TX_RT:
		bytes_left = min((int)count, MAX_RT_LENGTH);
		tx_data[1] = 0;
		break;
	case TAVARUA_TX_PS:
		bytes_left = min((int)count, MAX_PS_LENGTH);
		tx_data[4] = 0;
		break;
	default:
		FMDERR("%s: Unknown TX mode\n", __func__);
		return -1;
	}
	retval = sync_write_xfr(radio, radio->tx_mode, tx_data);
	if (retval < 0)
		return retval;

	/* send payload to FM hardware */
	while (bytes_left) {
		chunk_index++;
		bytes_to_copy = min(bytes_left, XFR_REG_NUM);
		if (copy_from_user(tx_data, data + bytes_copied, bytes_to_copy))
			return -EFAULT;
		retval = sync_write_xfr(radio, radio->tx_mode +
						chunk_index, tx_data);
		if (retval < 0)
			return retval;

		bytes_copied += bytes_to_copy;
		bytes_left -= bytes_to_copy;
	}

	/* send the header */
	switch (radio->tx_mode) {
	case TAVARUA_TX_RT:
		FMDBG("Writing RT header\n");
		tx_data[0] = bytes_copied;
		tx_data[1] = TX_ON | 0x03; /* on | PTY */
		tx_data[2] = 0x12; /* PI high */
		tx_data[3] = 0x34; /* PI low */
		break;
	case TAVARUA_TX_PS:
		FMDBG("Writing PS header\n");
		tx_data[0] = chunk_index;
		tx_data[1] = 0x03; /* PTY */
		tx_data[2] = 0x12; /* PI high */
		tx_data[3] = 0x34; /* PI low */
		tx_data[4] = TX_ON | 0x01;
		break;
	default:
		FMDERR("%s: Unknown TX mode\n", __func__);
		return -1;
	}
	retval = sync_write_xfr(radio, radio->tx_mode, tx_data);
	if (retval < 0)
		return retval;
	FMDBG("done writing: %d\n", retval);
	return bytes_copied;
}

/*=============================================================================
FUNCTION:  tavarua_fops_open
=============================================================================*/
/**
  This function is called when a process tries to open the device file, like
	"cat /dev/mycharfile"

  @param file: file descriptor.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int tavarua_fops_open(struct file *file)
{
	struct tavarua_device *radio = video_get_drvdata(video_devdata(file));
	int retval = -ENODEV;
	unsigned char value;
	/* FM core bring up */
	int i = 0;
	char fm_ctl0_part1[] = { 0xCA, 0xCE, 0xD6 };
	char fm_ctl1[] = { 0x03 };
	char fm_ctl0_part2[] = { 0xB6, 0xB7 };
	char buffer[] = {0x00, 0x48, 0x8A, 0x8E, 0x97, 0xB7};
	int bahama_present = -ENODEV;

	mutex_lock(&radio->lock);
	if (radio->users) {
		mutex_unlock(&radio->lock);
		return -EBUSY;
	} else {
		radio->users++;
	}
	mutex_unlock(&radio->lock);

	/* initial gpio pin config & Power up */
	retval = radio->pdata->fm_setup(radio->pdata);
	if (retval) {
		printk(KERN_ERR "%s: failed config gpio & pmic\n", __func__);
		goto open_err_setup;
	}
	/* enable irq */
	retval = tavarua_request_irq(radio);
	if (retval < 0) {
		printk(KERN_ERR "%s: failed to request irq\n", __func__);
		goto open_err_req_irq;
	}
	/* call top level marimba interface here to enable FM core */
	FMDBG("initializing SoC\n");

	bahama_present = is_bahama();

	if (bahama_present == -ENODEV)
		return -ENODEV;

	if (bahama_present)
		radio->marimba->mod_id = SLAVE_ID_BAHAMA;
	else
		radio->marimba->mod_id = MARIMBA_SLAVE_ID_MARIMBA;

	value = FM_ENABLE;
	retval = marimba_write_bit_mask(radio->marimba,
			MARIMBA_XO_BUFF_CNTRL, &value, 1, value);
	if (retval < 0) {
		printk(KERN_ERR "%s:XO_BUFF_CNTRL write failed\n",
					__func__);
		goto open_err_all;
	}


	/* Bring up FM core */
	if (bahama_present)	{

		radio->marimba->mod_id = SLAVE_ID_BAHAMA;
		/* Read the Bahama version*/
		retval = marimba_read_bit_mask(radio->marimba,
				0x00,  &bahama_version, 1, 0x1F);
		if (retval < 0) {
			printk(KERN_ERR "%s: version read failed",
				__func__);
			goto open_err_all;
		}
		/* Check for Bahama V2 variant*/
		if (bahama_version == 0x09)	{

			/* In case of Bahama v2, forcefully enable the
			 * internal analog and digital voltage controllers
			 */
			value = 0x06;
			/* value itself used as mask in these writes*/
			retval = marimba_write_bit_mask(radio->marimba,
			BAHAMA_LDO_DREG_CTL0, &value, 1, value);
			if (retval < 0) {
				printk(KERN_ERR "%s:0xF0 write failed\n",
					__func__);
				goto open_err_all;
			}
			value = 0x86;
			retval = marimba_write_bit_mask(radio->marimba,
				BAHAMA_LDO_AREG_CTL0, &value, 1, value);
			if (retval < 0) {
				printk(KERN_ERR "%s:0xF4 write failed\n",
					__func__);
				goto open_err_all;
			}
		}

		/*write FM mode*/
		retval = tavarua_write_register(radio, BAHAMA_FM_MODE_REG,
					BAHAMA_FM_MODE_NORMAL);
		if (retval < 0) {
			printk(KERN_ERR "failed to set the FM mode: %d\n",
					retval);
			goto open_err_all;
		}
		/*Write first sequence of bytes to FM_CTL0*/
		for (i = 0; i < 3; i++)  {
			retval = tavarua_write_register(radio,
					BAHAMA_FM_CTL0_REG, fm_ctl0_part1[i]);
			if (retval < 0) {
				printk(KERN_ERR "FM_CTL0:set-1 failure: %d\n",
							retval);
				goto open_err_all;
			}
		}
		/*Write the FM_CTL1 sequence*/
		for (i = 0; i < 1; i++)  {
			retval = tavarua_write_register(radio,
					BAHAMA_FM_CTL1_REG, fm_ctl1[i]);
			if (retval < 0) {
				printk(KERN_ERR "FM_CTL1 write failure: %d\n",
							retval);
				goto open_err_all;
			}
		}
		/*Write second sequence of bytes to FM_CTL0*/
		for (i = 0; i < 2; i++)  {
			retval = tavarua_write_register(radio,
					BAHAMA_FM_CTL0_REG, fm_ctl0_part2[i]);
			if (retval < 0) {
				printk(KERN_ERR "FM_CTL0:set-2 failure: %d\n",
					retval);
			goto open_err_all;
			}
		}
	} else {
		retval = tavarua_write_registers(radio, LEAKAGE_CNTRL,
						buffer, 6);
		if (retval < 0) {
			printk(KERN_ERR "%s: failed to bring up FM Core\n",
						__func__);
			goto open_err_all;
		}
	}
	/* Wait for interrupt i.e. complete(&radio->sync_req_done); call */
	/*Initialize the completion variable for
	for the proper behavior*/
	init_completion(&radio->sync_req_done);
	if (!wait_for_completion_timeout(&radio->sync_req_done,
		msecs_to_jiffies(wait_timeout))) {
		retval = -1;
		FMDERR("Timeout waiting for initialization\n");
	}

	/* get Chip ID */
	retval = tavarua_write_register(radio, XFRCTRL, CHIPID);
	if (retval < 0)
		goto open_err_all;
	msleep(TAVARUA_DELAY);
	tavarua_read_registers(radio, XFRCTRL, XFR_REG_NUM+1);
	if (radio->registers[XFRCTRL] != CHIPID)
		goto open_err_all;

	radio->chipID = (radio->registers[XFRCTRL+2] << 24) |
			(radio->registers[XFRCTRL+5] << 16) |
			(radio->registers[XFRCTRL+6] << 8)  |
			(radio->registers[XFRCTRL+7]);

	printk(KERN_WARNING DRIVER_NAME ": Chip ID %x\n", radio->chipID);
	if (radio->chipID == MARIMBA_A0) {
		printk(KERN_WARNING DRIVER_NAME ": Unsupported hardware: %x\n",
						radio->chipID);
		retval = -1;
		goto open_err_all;
	}

	radio->handle_irq = 0;
	marimba_set_fm_status(radio->marimba, true);
	return 0;


open_err_all:
    /*Disable FM in case of error*/
	value = 0x00;
	marimba_write_bit_mask(radio->marimba, MARIMBA_XO_BUFF_CNTRL,
							&value, 1, value);
	tavarua_disable_irq(radio);
open_err_req_irq:
	radio->pdata->fm_shutdown(radio->pdata);
open_err_setup:
	radio->handle_irq = 1;
	radio->users = 0;
	return retval;
}

/*=============================================================================
FUNCTION:  tavarua_fops_release
=============================================================================*/
/**
  This function is called when a process closes the device file.

  @param file: file descriptor.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int tavarua_fops_release(struct file *file)
{
	int retval;
	struct tavarua_device *radio = video_get_drvdata(video_devdata(file));
	unsigned char value;
	int i = 0;
	/*FM Core shutdown sequence for Bahama*/
	char fm_ctl0_part1[] = { 0xB7 };
	char fm_ctl1[] = { 0x03 };
	char fm_ctl0_part2[] = { 0x9F, 0x48, 0x02 };
	int bahama_present = -ENODEV;
	/*FM Core shutdown sequence for Marimba*/
	char buffer[] = {0x18, 0xB7, 0x48};
	bool bt_status = false;
	int index;
	/* internal regulator controllers DREG_CTL0, AREG_CTL0
	 * has to be kept in the valid state based on the bt status.
	 * 1st row is the state when no clients are active,
	 * and the second when bt is in on state.
	 */
	char internal_vreg_ctl[2][2] = {
		{ 0x04, 0x84 },
		{ 0x00, 0x80 }
	};

	if (!radio)
		return -ENODEV;
	FMDBG("In %s", __func__);

	/* disable radio ctrl */
	retval = tavarua_write_register(radio, RDCTRL, 0x00);

	FMDBG("%s, Disable IRQs\n", __func__);
	/* disable irq */
	retval = tavarua_disable_irq(radio);
	if (retval < 0) {
		printk(KERN_ERR "%s: failed to disable irq\n", __func__);
		return retval;
	}

	bahama_present = is_bahama();

	if (bahama_present == -ENODEV)
		return -ENODEV;

	if (bahama_present)	{
		/*Write first sequence of bytes to FM_CTL0*/
		for (i = 0; i < 1; i++) {
			retval = tavarua_write_register(radio,
					BAHAMA_FM_CTL0_REG, fm_ctl0_part1[i]);
			if (retval < 0) {
				printk(KERN_ERR "FM_CTL0:Set-1 failure: %d\n",
						retval);
				break;
			}
		}
		/*Write the FM_CTL1 sequence*/
		for (i = 0; i < 1; i++)  {
			retval = tavarua_write_register(radio,
					BAHAMA_FM_CTL1_REG, fm_ctl1[i]);
			if (retval < 0) {
				printk(KERN_ERR "FM_CTL1 failure: %d\n",
						retval);
				break;
			}
		}
		/*Write second sequence of bytes to FM_CTL0*/
		for (i = 0; i < 3; i++)   {
			retval = tavarua_write_register(radio,
					BAHAMA_FM_CTL0_REG, fm_ctl0_part2[i]);
			if (retval < 0) {
				printk(KERN_ERR "FM_CTL0:Set-2 failure: %d\n",
						retval);
			break;
			}
		}
	}	else	{

		retval = tavarua_write_registers(radio, FM_CTL0,
				buffer, sizeof(buffer)/sizeof(buffer[0]));
		if (retval < 0) {
			printk(KERN_ERR "%s: failed to bring down the  FM Core\n",
							__func__);
			return retval;
		}
	}

	bt_status = marimba_get_bt_status(radio->marimba);
	/* Set the index based on the bt status*/
	index = bt_status ?  1 : 0;
	/* Check for Bahama's existance and Bahama V2 variant*/
	if (bahama_present && (bahama_version == 0x09))   {
		radio->marimba->mod_id = SLAVE_ID_BAHAMA;
		/* actual value itself used as mask*/
		retval = marimba_write_bit_mask(radio->marimba,
			BAHAMA_LDO_DREG_CTL0, &internal_vreg_ctl[bt_status][0],
			 1, internal_vreg_ctl[index][0]);
		if (retval < 0) {
			printk(KERN_ERR "%s:0xF0 write failed\n", __func__);
			return retval;
		}
		/* actual value itself used as mask*/
		retval = marimba_write_bit_mask(radio->marimba,
			BAHAMA_LDO_AREG_CTL0, &internal_vreg_ctl[bt_status][1],
			1, internal_vreg_ctl[index][1]);
		if (retval < 0) {
			printk(KERN_ERR "%s:0xF4 write failed\n", __func__);
			return retval;
		}
	} else    {
		/* disable fm core */
		radio->marimba->mod_id = MARIMBA_SLAVE_ID_MARIMBA;
	}

	value = 0x00;
	retval = marimba_write_bit_mask(radio->marimba, MARIMBA_XO_BUFF_CNTRL,
							&value, 1, FM_ENABLE);
	if (retval < 0) {
		printk(KERN_ERR "%s:XO_BUFF_CNTRL write failed\n", __func__);
		return retval;
	}
	FMDBG("%s, Calling fm_shutdown\n", __func__);
	/* teardown gpio and pmic */
	radio->pdata->fm_shutdown(radio->pdata);
	radio->handle_irq = 1;
	radio->users = 0;
	marimba_set_fm_status(radio->marimba, false);
	return 0;
}

/*
 * tavarua_fops - file operations interface
 */
static const struct v4l2_file_operations tavarua_fops = {
	.owner = THIS_MODULE,
	.read = tavarua_fops_read,
	.write = tavarua_fops_write,
	.ioctl = video_ioctl2,
	.open  = tavarua_fops_open,
	.release = tavarua_fops_release,
};

/*************************************************************************
 * Video4Linux Interface
 *************************************************************************/

/*
 * tavarua_v4l2_queryctrl - query control
 */
static struct v4l2_queryctrl tavarua_v4l2_queryctrl[] = {
	{
		.id	       = V4L2_CID_AUDIO_VOLUME,
		.type	       = V4L2_CTRL_TYPE_INTEGER,
		.name	       = "Volume",
		.minimum       = 0,
		.maximum       = 15,
		.step	       = 1,
		.default_value = 15,
	},
	{
		.id	       = V4L2_CID_AUDIO_BALANCE,
		.flags	       = V4L2_CTRL_FLAG_DISABLED,
	},
	{
		.id	       = V4L2_CID_AUDIO_BASS,
		.flags	       = V4L2_CTRL_FLAG_DISABLED,
	},
	{
		.id	       = V4L2_CID_AUDIO_TREBLE,
		.flags	       = V4L2_CTRL_FLAG_DISABLED,
	},
	{
		.id	       = V4L2_CID_AUDIO_MUTE,
		.type	       = V4L2_CTRL_TYPE_BOOLEAN,
		.name	       = "Mute",
		.minimum       = 0,
		.maximum       = 1,
		.step	       = 1,
		.default_value = 1,
	},
	{
		.id	       = V4L2_CID_AUDIO_LOUDNESS,
		.flags	       = V4L2_CTRL_FLAG_DISABLED,
	},
	{
		.id	       = V4L2_CID_PRIVATE_TAVARUA_SRCHMODE,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name	       = "Search mode",
		.minimum       = 0,
		.maximum       = 7,
		.step	       = 1,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_SCANDWELL,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name          = "Search dwell time",
		.minimum       = 0,
		.maximum       = 7,
		.step          = 1,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_SRCHON,
		.type          = V4L2_CTRL_TYPE_BOOLEAN,
		.name          = "Search on/off",
		.minimum       = 0,
		.maximum       = 1,
		.step          = 1,
		.default_value = 1,

	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_STATE,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name          = "radio 0ff/rx/tx/reset",
		.minimum       = 0,
		.maximum       = 3,
		.step          = 1,
		.default_value = 1,

	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_REGION,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name          = "radio standard",
		.minimum       = 0,
		.maximum       = 2,
		.step          = 1,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_SIGNAL_TH,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name          = "Signal Threshold",
		.minimum       = 0x80,
		.maximum       = 0x7F,
		.step          = 1,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_SRCH_PTY,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name          = "Search PTY",
		.minimum       = 0,
		.maximum       = 31,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_SRCH_PI,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name          = "Search PI",
		.minimum       = 0,
		.maximum       = 0xFF,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_SRCH_CNT,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name          = "Preset num",
		.minimum       = 0,
		.maximum       = 12,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_EMPHASIS,
		.type          = V4L2_CTRL_TYPE_BOOLEAN,
		.name          = "Emphasis",
		.minimum       = 0,
		.maximum       = 1,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_RDS_STD,
		.type          = V4L2_CTRL_TYPE_BOOLEAN,
		.name          = "RDS standard",
		.minimum       = 0,
		.maximum       = 1,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_SPACING,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name          = "Channel spacing",
		.minimum       = 0,
		.maximum       = 2,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_RDSON,
		.type          = V4L2_CTRL_TYPE_BOOLEAN,
		.name          = "RDS on/off",
		.minimum       = 0,
		.maximum       = 1,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_RDSGROUP_MASK,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name          = "RDS group mask",
		.minimum       = 0,
		.maximum       = 0xFFFFFFFF,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_RDSGROUP_PROC,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name          = "RDS processing",
		.minimum       = 0,
		.maximum       = 0xFF,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_RDSD_BUF,
		.type          = V4L2_CTRL_TYPE_INTEGER,
		.name          = "RDS data groups to buffer",
		.minimum       = 1,
		.maximum       = 21,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_PSALL,
		.type          = V4L2_CTRL_TYPE_BOOLEAN,
		.name          = "pass all ps strings",
		.minimum       = 0,
		.maximum       = 1,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_LP_MODE,
		.type          = V4L2_CTRL_TYPE_BOOLEAN,
		.name          = "Low power mode",
		.minimum       = 0,
		.maximum       = 1,
		.default_value = 0,
	},
	{
		.id            = V4L2_CID_PRIVATE_TAVARUA_ANTENNA,
		.type          = V4L2_CTRL_TYPE_BOOLEAN,
		.name          = "headset/internal",
		.minimum       = 0,
		.maximum       = 1,
		.default_value = 0,
	}
};

/*=============================================================================
FUNCTION:  tavarua_vidioc_querycap
=============================================================================*/
/**
  This function is called to query device capabilities.

  NOTE:
  All V4L2 devices support the VIDIOC_QUERYCAP ioctl. It is used to identify
  kernel devices compatible with this specification and to obtain information
  about driver and hardware capabilities. The ioctl takes a pointer to a struct
  v4l2_capability which is filled by the driver. When the driver is not
  compatible with this specification the ioctl returns an EINVAL error code.

  @param file: File descriptor returned by open().
  @param capability: pointer to struct v4l2_capability.

  @return On success 0 is returned, else error code.
  @return EINVAL: The device is not compatible with this specification.
*/
static int tavarua_vidioc_querycap(struct file *file, void *priv,
		struct v4l2_capability *capability)
{
	struct tavarua_device *radio = video_get_drvdata(video_devdata(file));

	strlcpy(capability->driver, DRIVER_NAME, sizeof(capability->driver));
	strlcpy(capability->card, DRIVER_CARD, sizeof(capability->card));
	sprintf(capability->bus_info, "I2C");
	capability->capabilities = V4L2_CAP_TUNER | V4L2_CAP_RADIO;

	capability->version = radio->chipID;

	return 0;
}

/*=============================================================================
FUNCTION:  tavarua_vidioc_queryctrl
=============================================================================*/
/**
  This function is called to query the device and driver for supported video
  controls (enumerate control items).

  NOTE:
  To query the attributes of a control, the applications set the id field of
  a struct v4l2_queryctrl and call the VIDIOC_QUERYCTRL ioctl with a pointer
  to this structure. The driver fills the rest of the structure or returns an
  EINVAL error code when the id is invalid.

  @param file: File descriptor returned by open().
  @param qc: pointer to struct v4l2_queryctrl.

  @return On success 0 is returned, else error code.
  @return EINVAL: The struct v4l2_queryctrl id is invalid.
*/
static int tavarua_vidioc_queryctrl(struct file *file, void *priv,
		struct v4l2_queryctrl *qc)
{
	unsigned char i;
	int retval = -EINVAL;

	for (i = 0; i < ARRAY_SIZE(tavarua_v4l2_queryctrl); i++) {
		if (qc->id && qc->id == tavarua_v4l2_queryctrl[i].id) {
			memcpy(qc, &(tavarua_v4l2_queryctrl[i]), sizeof(*qc));
			retval = 0;
			break;
		}
	}
	if (retval < 0)
		printk(KERN_WARNING DRIVER_NAME
			": query conv4ltrol failed with %d\n", retval);

	return retval;
}

/*=============================================================================
FUNCTION:  tavarua_vidioc_g_ctrl
=============================================================================*/
/**
  This function is called to get the value of a control.

  NOTE:
  To get the current value of a control, applications initialize the id field
  of a struct v4l2_control and call the VIDIOC_G_CTRL ioctl with a pointer to
  this structure.

  When the id is invalid drivers return an EINVAL error code. When the value is
  out of bounds drivers can choose to take the closest valid value or return an
  ERANGE error code, whatever seems more appropriate.

  @param file: File descriptor returned by open().
  @param ctrl: pointer to struct v4l2_control.

  @return On success 0 is returned, else error code.
  @return EINVAL: The struct v4l2_control id is invalid.
  @return ERANGE: The struct v4l2_control value is out of bounds.
  @return EBUSY: The control is temporarily not changeable, possibly because
  another applications took over control of the device function this control
  belongs to.
*/
static int tavarua_vidioc_g_ctrl(struct file *file, void *priv,
		struct v4l2_control *ctrl)
{
	struct tavarua_device *radio = video_get_drvdata(video_devdata(file));
	int retval = 0;
	unsigned char xfr_buf[XFR_REG_NUM];
	signed char cRmssiThreshold;

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_VOLUME:
		break;
	case V4L2_CID_AUDIO_MUTE:
		ctrl->value = radio->registers[IOCTRL] & 0x03 ;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SRCHMODE:
		ctrl->value = radio->registers[SRCHCTRL] & SRCH_MODE;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SCANDWELL:
		ctrl->value = (radio->registers[SRCHCTRL] & SCAN_DWELL) >> 4;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SRCHON:
		ctrl->value = (radio->registers[SRCHCTRL] & SRCH_ON) >> 7 ;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_STATE:
		ctrl->value = (radio->registers[RDCTRL] & 0x03);
		break;
	case V4L2_CID_PRIVATE_TAVARUA_REGION:
		ctrl->value = radio->region_params.region;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SIGNAL_TH:
		retval = sync_read_xfr(radio, RX_CONFIG, xfr_buf);
		if (retval < 0) {
			FMDBG("[G IOCTL=V4L2_CID_PRIVATE_TAVARUA_SIGNAL_TH]\n");
			FMDBG("sync_read_xfr error: [retval=%d]\n", retval);
			break;
		}
		/* Since RMSSI Threshold is signed value */
		cRmssiThreshold = (signed char)xfr_buf[0];
		ctrl->value  = cRmssiThreshold;
		FMDBG("cRmssiThreshold: %d\n", cRmssiThreshold);
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SRCH_PTY:
		ctrl->value = radio->srch_params.srch_pty;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SRCH_PI:
		ctrl->value = radio->srch_params.srch_pi;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SRCH_CNT:
		ctrl->value = radio->srch_params.preset_num;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_EMPHASIS:
		ctrl->value = radio->region_params.emphasis;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_RDS_STD:
		ctrl->value = radio->region_params.rds_std;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SPACING:
		ctrl->value = radio->region_params.spacing;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_RDSON:
		ctrl->value = radio->registers[RDSCTRL] & RDS_ON;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_RDSGROUP_MASK:
		retval = sync_read_xfr(radio, RDS_CONFIG, xfr_buf);
		if (retval > -1)
			ctrl->value =   (xfr_buf[8] << 24) |
					(xfr_buf[9] << 16) |
					(xfr_buf[10] << 8) |
					 xfr_buf[11];
		break;
	case V4L2_CID_PRIVATE_TAVARUA_RDSGROUP_PROC:
		retval = tavarua_read_registers(radio, ADVCTRL, 1);
		if (retval > -1)
			ctrl->value = radio->registers[ADVCTRL];
		break;
	case V4L2_CID_PRIVATE_TAVARUA_RDSD_BUF:
		retval = sync_read_xfr(radio, RDS_CONFIG, xfr_buf);
		if (retval > -1)
			ctrl->value = xfr_buf[1];
		break;
	case V4L2_CID_PRIVATE_TAVARUA_PSALL:
		retval = sync_read_xfr(radio, RDS_CONFIG, xfr_buf);
		if (retval > -1)
			ctrl->value = xfr_buf[12] & RDS_CONFIG_PSALL;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_LP_MODE:
		ctrl->value = radio->lp_mode;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_ANTENNA:
		ctrl->value = GET_REG_FIELD(radio->registers[IOCTRL],
			IOC_ANTENNA_OFFSET, IOC_ANTENNA_MASK);
		break;
	default:
		retval = -EINVAL;
	}
	if (retval < 0)
		printk(KERN_WARNING DRIVER_NAME
		": get control failed with %d, id: %d\n", retval, ctrl->id);

	return retval;
}

/*=============================================================================
FUNCTION:  tavarua_vidioc_s_ctrl
=============================================================================*/
/**
  This function is called to set the value of a control.

  NOTE:
  To change the value of a control, applications initialize the id and value
  fields of a struct v4l2_control and call the VIDIOC_S_CTRL ioctl.

  When the id is invalid drivers return an EINVAL error code. When the value is
  out of bounds drivers can choose to take the closest valid value or return an
  ERANGE error code, whatever seems more appropriate.

  @param file: File descriptor returned by open().
  @param ctrl: pointer to struct v4l2_control.

  @return On success 0 is returned, else error code.
  @return EINVAL: The struct v4l2_control id is invalid.
  @return ERANGE: The struct v4l2_control value is out of bounds.
  @return EBUSY: The control is temporarily not changeable, possibly because
  another applications took over control of the device function this control
  belongs to.
*/
static int tavarua_vidioc_s_ctrl(struct file *file, void *priv,
		struct v4l2_control *ctrl)
{
	struct tavarua_device *radio = video_get_drvdata(video_devdata(file));
	int retval = 0;
	unsigned char value;
	unsigned char xfr_buf[XFR_REG_NUM];

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_VOLUME:
		break;
	case V4L2_CID_AUDIO_MUTE:
		value = (radio->registers[IOCTRL] & ~IOC_HRD_MUTE) |
							(ctrl->value & 0x03);
		retval = tavarua_write_register(radio, IOCTRL, value);
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SRCHMODE:
		value = (radio->registers[SRCHCTRL] & ~SRCH_MODE) |
							ctrl->value;
		radio->registers[SRCHCTRL] = value;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SCANDWELL:
		value = (radio->registers[SRCHCTRL] & ~SCAN_DWELL) |
						(ctrl->value << 4);
		radio->registers[SRCHCTRL] = value;
		break;
	/* start/stop search */
	case V4L2_CID_PRIVATE_TAVARUA_SRCHON:
		FMDBG("starting search\n");
		tavarua_search(radio, ctrl->value, SRCH_DIR_UP);
		break;
	case V4L2_CID_PRIVATE_TAVARUA_STATE:
		/* check if already on */
		radio->handle_irq = 1;
		if ((ctrl->value == FM_RECV) && !(radio->registers[RDCTRL] &
							FM_RECV)) {
			FMDBG("clearing flags\n");
			init_completion(&radio->sync_xfr_start);
			init_completion(&radio->sync_req_done);
			radio->xfr_in_progress = 0;
			radio->xfr_bytes_left = 0;
			FMDBG("turning on ..\n");
			retval = tavarua_start(radio, FM_RECV);
			if (retval >= 0) {
				FMDBG("Setting audio path ...\n");
				retval = tavarua_set_audio_path(
					TAVARUA_AUDIO_OUT_DIGITAL_ON,
					TAVARUA_AUDIO_OUT_ANALOG_OFF);
				if (retval < 0) {
					FMDERR("Error in tavarua_set_audio_path"
						" %d\n", retval);
				}

                //@fihtdc, set default threshold
                if(sync_read_xfr(radio, RX_CONFIG, xfr_buf)) {
                    xfr_buf[0] = (unsigned char)TAVARUA_DEFAULT_TH;
                    xfr_buf[1] = (unsigned char)TAVARUA_DEFAULT_TH;
                    xfr_buf[4] = 0x01;
                    sync_write_xfr(radio, RX_CONFIG, xfr_buf);
                }

			 /* Enabling 'SoftMute' and 'SignalBlending' features */
			value = (radio->registers[IOCTRL] |
				    IOC_SFT_MUTE | IOC_SIG_BLND);
			retval = tavarua_write_register(radio, IOCTRL, value);
			if (retval < 0)
				FMDBG("SMute and SBlending not enabled\n");
			}
		}
		/* check if off */
		else if ((ctrl->value == FM_OFF) && radio->registers[RDCTRL]) {
			FMDBG("turning off...\n");
			retval = tavarua_write_register(radio, RDCTRL,
							ctrl->value);
			/*Make it synchronous
			Block it till READY interrupt
			Wait for interrupt i.e.
			complete(&radio->sync_req_done)
			*/

			if (retval >= 0) {
				if (!wait_for_completion_timeout(
					&radio->sync_req_done,
					msecs_to_jiffies(wait_timeout)))
					FMDBG("turning off timedout...\n");
			}
		} else if ((ctrl->value == FM_TRANS) &&
			   ((radio->registers[RDCTRL] & 0x03) != FM_TRANS)) {
			FMDBG("transmit mode\n");
			retval = tavarua_start(radio, FM_TRANS);
		}
		break;
	case V4L2_CID_PRIVATE_TAVARUA_REGION:
		retval = tavarua_set_region(radio, ctrl->value);
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SIGNAL_TH:
		retval = sync_read_xfr(radio, RX_CONFIG, xfr_buf);
		if (retval < 0)	{
			FMDERR("V4L2_CID_PRIVATE_TAVARUA_SIGNAL_TH]\n");
			FMDERR("sync_read_xfr [retval=%d]\n", retval);
			break;
		}
		/* RMSSI Threshold is a signed 8 bit value */
		xfr_buf[0] = (unsigned char)ctrl->value;
		xfr_buf[1] = (unsigned char)ctrl->value;
		xfr_buf[4] = 0x01;
		retval = sync_write_xfr(radio, RX_CONFIG, xfr_buf);
		if (retval < 0) {
			FMDERR("V4L2_CID_PRIVATE_TAVARUA_SIGNAL_TH]\n");
			FMDERR("sync_write_xfr [retval=%d]\n", retval);
			break;
		}
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SRCH_PTY:
		radio->srch_params.srch_pty = ctrl->value;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SRCH_PI:
		radio->srch_params.srch_pi = ctrl->value;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SRCH_CNT:
		radio->srch_params.preset_num = ctrl->value;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_EMPHASIS:
		radio->region_params.emphasis = ctrl->value;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_RDS_STD:
		radio->region_params.rds_std = ctrl->value;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_SPACING:
		radio->region_params.spacing = ctrl->value;
		break;
	case V4L2_CID_PRIVATE_TAVARUA_RDSON:
		retval = 0;
		if (ctrl->value != (radio->registers[RDSCTRL] & RDS_ON)) {
			value = radio->registers[RDSCTRL] | ctrl->value;
			retval = tavarua_write_register(radio, RDSCTRL, value);
		}
		break;
	case V4L2_CID_PRIVATE_TAVARUA_RDSGROUP_MASK:
		retval = sync_read_xfr(radio, RDS_CONFIG, xfr_buf);
		if (retval < 0)
			break;
		xfr_buf[8] = (ctrl->value & 0xFF000000) >> 24;
		xfr_buf[9] = (ctrl->value & 0x00FF0000) >> 16;
		xfr_buf[10] = (ctrl->value & 0x0000FF00) >> 8;
		xfr_buf[11] = (ctrl->value & 0x000000FF);
		retval = sync_write_xfr(radio, RDS_CONFIG, xfr_buf);
		break;
	case V4L2_CID_PRIVATE_TAVARUA_RDSGROUP_PROC:
		value  = radio->registers[ADVCTRL] | ctrl->value  ;
		retval = tavarua_write_register(radio, ADVCTRL, value);
		break;
	case V4L2_CID_PRIVATE_TAVARUA_RDSD_BUF:
		retval = sync_read_xfr(radio, RDS_CONFIG, xfr_buf);
		if (retval < 0)
			break;
		xfr_buf[1] = ctrl->value;
		retval = sync_write_xfr(radio, RDS_CONFIG, xfr_buf);
		break;
	case V4L2_CID_PRIVATE_TAVARUA_PSALL:
		retval = sync_read_xfr(radio, RDS_CONFIG, xfr_buf);
		value = ctrl->value & RDS_CONFIG_PSALL;
		if (retval < 0)
			break;
		xfr_buf[12] &= ~RDS_CONFIG_PSALL;
		xfr_buf[12] |= value;
		retval = sync_write_xfr(radio, RDS_CONFIG, xfr_buf);
		break;
	case V4L2_CID_PRIVATE_TAVARUA_LP_MODE:
		retval = 0;
		if (ctrl->value == radio->lp_mode)
			break;
		if (ctrl->value) {
			FMDBG("going into low power mode\n");
			retval = tavarua_disable_interrupts(radio);
		} else {
			FMDBG("going into normal power mode\n");
			tavarua_setup_interrupts(radio,
				(radio->registers[RDCTRL] & 0x03));
		}
		break;
	case V4L2_CID_PRIVATE_TAVARUA_ANTENNA:
		SET_REG_FIELD(radio->registers[IOCTRL], ctrl->value,
					IOC_ANTENNA_OFFSET, IOC_ANTENNA_MASK);
		break;
	default:
		retval = -EINVAL;
	}
	if (retval < 0)
		printk(KERN_WARNING DRIVER_NAME
		": set control failed with %d, id : %d\n", retval, ctrl->id);

	return retval;
}

/*=============================================================================
FUNCTION:  tavarua_vidioc_g_tuner
=============================================================================*/
/**
  This function is called to get tuner attributes.

  NOTE:
  To query the attributes of a tuner, applications initialize the index field
  and zero out the reserved array of a struct v4l2_tuner and call the
  VIDIOC_G_TUNER ioctl with a pointer to this structure. Drivers fill the rest
  of the structure or return an EINVAL error code when the index is out of
  bounds. To enumerate all tuners applications shall begin at index zero,
  incrementing by one until the driver returns EINVAL.

  @param file: File descriptor returned by open().
  @param tuner: pointer to struct v4l2_tuner.

  @return On success 0 is returned, else error code.
  @return EINVAL: The struct v4l2_tuner index is out of bounds.
*/
static int tavarua_vidioc_g_tuner(struct file *file, void *priv,
		struct v4l2_tuner *tuner)
{
	struct tavarua_device *radio = video_get_drvdata(video_devdata(file));
	int retval;

	if (tuner->index > 0)
		return -EINVAL;

	/* read status rssi */
	retval = tavarua_read_registers(radio, IOCTRL, 1);
	if (retval < 0)
		return retval;
	/* read RMSSI */
	retval = tavarua_read_registers(radio, RMSSI, 1);
	if (retval < 0)
		return retval;

	strcpy(tuner->name, "FM");
	tuner->type = V4L2_TUNER_RADIO;
	tuner->rangelow  =  radio->region_params.band_low;
	tuner->rangehigh =  radio->region_params.band_high;
	tuner->rxsubchans = V4L2_TUNER_SUB_MONO | V4L2_TUNER_SUB_STEREO;
	tuner->capability = V4L2_TUNER_CAP_LOW;
	tuner->signal = radio->registers[RMSSI];

	/* Stereo indicator == Stereo (instead of Mono) */
	if (radio->registers[IOCTRL] & IOC_MON_STR)
		tuner->audmode = V4L2_TUNER_MODE_STEREO;
	else
	  tuner->audmode = V4L2_TUNER_MODE_MONO;

	/* automatic frequency control: -1: freq to low, 1 freq to high */
	tuner->afc = 0;

	return 0;
}

/*=============================================================================
FUNCTION:  tavarua_vidioc_s_tuner
=============================================================================*/
/**
  This function is called to set tuner attributes. Used to set mono/stereo mode.

  NOTE:
  Tuners have two writable properties, the audio mode and the radio frequency.
  To change the audio mode, applications initialize the index, audmode and
  reserved fields and call the VIDIOC_S_TUNER ioctl. This will not change the
  current tuner, which is determined by the current video input. Drivers may
  choose a different audio mode if the requested mode is invalid or unsupported.
  Since this is a write-only ioctl, it does not return the actually selected
  audio mode.

  To change the radio frequency the VIDIOC_S_FREQUENCY ioctl is available.

  @param file: File descriptor returned by open().
  @param tuner: pointer to struct v4l2_tuner.

  @return On success 0 is returned, else error code.
  @return -EINVAL: The struct v4l2_tuner index is out of bounds.
*/
static int tavarua_vidioc_s_tuner(struct file *file, void *priv,
		struct v4l2_tuner *tuner)
{
	struct tavarua_device *radio = video_get_drvdata(video_devdata(file));
	int retval;
	int audmode;
	if (tuner->index > 0)
		return -EINVAL;

	FMDBG("%s: set low to %d\n", __func__, tuner->rangelow);
	radio->region_params.band_low = tuner->rangelow;
	radio->region_params.band_high = tuner->rangehigh;
	if (tuner->audmode == V4L2_TUNER_MODE_MONO)
		/* Mono */
		audmode = (radio->registers[IOCTRL] | IOC_MON_STR);
	 else
		/* Stereo */
		audmode = (radio->registers[IOCTRL] & ~IOC_MON_STR);
	retval = tavarua_write_register(radio, IOCTRL, audmode);
	if (retval < 0)
		printk(KERN_WARNING DRIVER_NAME
			": set tuner failed with %d\n", retval);

	return retval;
}

/*=============================================================================
FUNCTION:  tavarua_vidioc_g_frequency
=============================================================================*/
/**
  This function is called to get tuner or modulator radio frequency.

  NOTE:
  To get the current tuner or modulator radio frequency applications set the
  tuner field of a struct v4l2_frequency to the respective tuner or modulator
  number (only input devices have tuners, only output devices have modulators),
  zero out the reserved array and call the VIDIOC_G_FREQUENCY ioctl with a
  pointer to this structure. The driver stores the current frequency in the
  frequency field.

  @param file: File descriptor returned by open().
  @param freq: pointer to struct v4l2_frequency. This will be set to the
   resultant
  frequency in 62.5 khz on success.

  @return On success 0 is returned, else error code.
  @return EINVAL: The tuner index is out of bounds or the value in the type
  field is wrong.
*/
static int tavarua_vidioc_g_frequency(struct file *file, void *priv,
		struct v4l2_frequency *freq)
{
	struct tavarua_device *radio = video_get_drvdata(video_devdata(file));
	freq->type = V4L2_TUNER_RADIO;
	return tavarua_get_freq(radio, freq);

}

/*=============================================================================
FUNCTION:  tavarua_vidioc_s_frequency
=============================================================================*/
/**
  This function is called to set tuner or modulator radio frequency.

  NOTE:
  To change the current tuner or modulator radio frequency applications
  initialize the tuner, type and frequency fields, and the reserved array of
  a struct v4l2_frequency and call the VIDIOC_S_FREQUENCY ioctl with a pointer
  to this structure. When the requested frequency is not possible the driver
  assumes the closest possible value. However VIDIOC_S_FREQUENCY is a
  write-only ioctl, it does not return the actual new frequency.

  @param file: File descriptor returned by open().
  @param freq: pointer to struct v4l2_frequency.

  @return On success 0 is returned, else error code.
  @return EINVAL: The tuner index is out of bounds or the value in the type
  field is wrong.
*/
static int tavarua_vidioc_s_frequency(struct file *file, void *priv,
					struct v4l2_frequency *freq)
{
	struct tavarua_device *radio = video_get_drvdata(video_devdata(file));
	int retval = -1;
	struct v4l2_frequency getFreq;

	FMDBG("%s\n", __func__);

	if (freq->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	FMDBG("Calling tavarua_set_freq\n");

	INIT_COMPLETION(radio->sync_req_done);
	retval = tavarua_set_freq(radio, freq->frequency);
	if (retval < 0) {
		printk(KERN_WARNING DRIVER_NAME
			": set frequency failed with %d\n", retval);
	} else {
		/* Wait for interrupt i.e. complete
		(&radio->sync_req_done); call */
		if (!wait_for_completion_timeout(&radio->sync_req_done,
			msecs_to_jiffies(wait_timeout))) {
			FMDERR("Timeout: No Tune response");
			retval = tavarua_get_freq(radio, &getFreq);
			radio->tune_req = 0;
			if (retval > 0) {
				if (getFreq.frequency == freq->frequency) {
					/** This is success, queut the event*/
					tavarua_q_event(radio,
						TAVARUA_EVT_TUNE_SUCC);
					return 0;
				} else {
					return -EIO;
				}
			}
		}
	}
	radio->tune_req = 0;
	return retval;
}

/*=============================================================================
FUNCTION:  tavarua_vidioc_dqbuf
=============================================================================*/
/**
  This function is called to exchange a buffer with the driver.
  This is main buffer function, in essense its equivalent to a blocking
  read call.

  Applications call the VIDIOC_DQBUF ioctl to dequeue a filled (capturing) or
  displayed (output) buffer from the driver's outgoing queue. They just set
  the type and memory fields of a struct v4l2_buffer as above, when VIDIOC_DQBUF
  is called with a pointer to this structure the driver fills the remaining
  fields or returns an error code.

  NOTE:
  By default VIDIOC_DQBUF blocks when no buffer is in the outgoing queue.
  When the O_NONBLOCK flag was given to the open() function, VIDIOC_DQBUF
  returns immediately with an EAGAIN error code when no buffer is available.

  @param file: File descriptor returned by open().
  @param buffer: pointer to struct v4l2_buffer.

  @return On success 0 is returned, else error code.
  @return EAGAIN: Non-blocking I/O has been selected using O_NONBLOCK and no
  buffer was in the outgoing queue.
  @return EINVAL: The buffer type is not supported, or the index is out of
  bounds, or no buffers have been allocated yet, or the userptr or length are
  invalid.
  @return ENOMEM: Not enough physical or virtual memory was available to enqueue
  a user pointer buffer.
  @return EIO: VIDIOC_DQBUF failed due to an internal error. Can also indicate
  temporary problems like signal loss. Note the driver might dequeue an (empty)
  buffer despite returning an error, or even stop capturing.
*/
static int tavarua_vidioc_dqbuf(struct file *file, void *priv,
				struct v4l2_buffer *buffer)
{

	struct tavarua_device  *radio = video_get_drvdata(video_devdata(file));
	enum tavarua_buf_t buf_type = buffer->index;
	struct kfifo *data_fifo;
	unsigned char *buf = (unsigned char *)buffer->m.userptr;
	unsigned int len = buffer->length;
    unsigned int nonecopy=0;
    char *tbuf;

	FMDBG("%s: requesting buffer %d\n", __func__, buf_type);
	/* check if we can access the user buffer */
	if (!access_ok(VERIFY_WRITE, buf, len))
		return -EFAULT;
	if ((buf_type < TAVARUA_BUF_MAX) && (buf_type >= 0)) {
		data_fifo = &radio->data_buf[buf_type];
		if (buf_type == TAVARUA_BUF_EVENTS) {
			if (wait_event_interruptible(radio->event_queue,
				kfifo_len(data_fifo)) < 0) {
				return -EINTR;
			}
		}
	} else {
		FMDERR("invalid buffer type\n");
		return -EINVAL;
	}
    /* +++ AlbertYCFang, 2011.07.21 +++ */
	/*buffer->bytesused = kfifo_out_locked(data_fifo, buf, len, 
					&radio->buf_lock[buf_type]);*/
    tbuf = kzalloc(len, GFP_KERNEL);
    if (!tbuf)
    {
        FMDBG("**%s kzalloc fail!", __func__);
        return -EFAULT;
    }

	buffer->bytesused = kfifo_out_locked(data_fifo, tbuf, len, 
					&radio->buf_lock[buf_type]);
    nonecopy = copy_to_user(buf, tbuf, buffer->bytesused);

    if (nonecopy>0)
    {
        FMDBG("**Bytes that could not be copied = %d", nonecopy);
    }
    kfree(tbuf);
    /* --- AlbertYCFang, 2011.07.21 --- */

	return 0;
}

/*=============================================================================
FUNCTION:  tavarua_vidioc_g_fmt_type_private
=============================================================================*/
/**
  This function is here to make the v4l2 framework happy.
  We cannot use private buffers without it.

  @param file: File descriptor returned by open().
  @param f: pointer to struct v4l2_format.

  @return On success 0 is returned, else error code.
  @return EINVAL: The tuner index is out of bounds or the value in the type
  field is wrong.
*/
static int tavarua_vidioc_g_fmt_type_private(struct file *file, void *priv,
						struct v4l2_format *f)
{
	return 0;

}

/*=============================================================================
FUNCTION:  tavarua_vidioc_s_hw_freq_seek
=============================================================================*/
/**
  This function is called to perform a hardware frequency seek.

  Start a hardware frequency seek from the current frequency. To do this
  applications initialize the tuner, type, seek_upward and wrap_around fields,
  and zero out the reserved array of a struct v4l2_hw_freq_seek and call the
  VIDIOC_S_HW_FREQ_SEEK ioctl with a pointer to this structure.

  This ioctl is supported if the V4L2_CAP_HW_FREQ_SEEK capability is set.

  @param file: File descriptor returned by open().
  @param seek: pointer to struct v4l2_hw_freq_seek.

  @return On success 0 is returned, else error code.
  @return EINVAL: The tuner index is out of bounds or the value in the type
  field is wrong.
  @return EAGAIN: The ioctl timed-out. Try again.
*/
static int tavarua_vidioc_s_hw_freq_seek(struct file *file, void *priv,
					struct v4l2_hw_freq_seek *seek)
{
	struct tavarua_device  *radio = video_get_drvdata(video_devdata(file));
	int dir;
	if (seek->seek_upward)
		dir = SRCH_DIR_UP;
	else
		dir = SRCH_DIR_DOWN;
	FMDBG("starting search\n");
	return tavarua_search(radio, CTRL_ON, dir);
}

/*
 * tavarua_viddev_tamples - video device interface
 */
static const struct v4l2_ioctl_ops tavarua_ioctl_ops = {
	.vidioc_querycap              = tavarua_vidioc_querycap,
	.vidioc_queryctrl             = tavarua_vidioc_queryctrl,
	.vidioc_g_ctrl                = tavarua_vidioc_g_ctrl,
	.vidioc_s_ctrl                = tavarua_vidioc_s_ctrl,
	.vidioc_g_tuner               = tavarua_vidioc_g_tuner,
	.vidioc_s_tuner               = tavarua_vidioc_s_tuner,
	.vidioc_g_frequency           = tavarua_vidioc_g_frequency,
	.vidioc_s_frequency           = tavarua_vidioc_s_frequency,
	.vidioc_s_hw_freq_seek        = tavarua_vidioc_s_hw_freq_seek,
	.vidioc_dqbuf                 = tavarua_vidioc_dqbuf,
	.vidioc_g_fmt_type_private    = tavarua_vidioc_g_fmt_type_private,
};

static struct video_device tavarua_viddev_template = {
	.fops                   = &tavarua_fops,
	.ioctl_ops              = &tavarua_ioctl_ops,
	.name                   = DRIVER_NAME,
	.release                = video_device_release,
};

/*==============================================================
FUNCTION:  FmQSocCom_EnableInterrupts
==============================================================*/
/**
  This function enable interrupts.

  @param radio: structure pointer passed by client.
  @param state: FM radio state (receiver/transmitter/off/reset).

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int tavarua_setup_interrupts(struct tavarua_device *radio,
					enum radio_state_t state)
{
	int retval;
	unsigned char int_ctrl[XFR_REG_NUM];

	if (!radio->lp_mode)
		return 0;

	int_ctrl[STATUS_REG1] = READY | TUNE | SEARCH | SCANNEXT |
				SIGNAL | INTF | SYNC | AUDIO;
	if (state == FM_RECV)
		int_ctrl[STATUS_REG2] =  RDSDAT | RDSRT | RDSPS | RDSAF;
	else
		int_ctrl[STATUS_REG2] = RDSRT | TXRDSDAT | TXRDSDONE;

	int_ctrl[STATUS_REG3] = TRANSFER | ERROR;

	/* use xfr for interrupt setup */
    if (radio->chipID == MARIMBA_2_1 || radio->chipID == BAHAMA_1_0
		|| radio->chipID == BAHAMA_2_0) {
		FMDBG("Setting interrupts\n");
		retval =  sync_write_xfr(radio, INT_CTRL, int_ctrl);
	/* use register write to setup interrupts */
	} else {
		retval = tavarua_write_register(radio,
					STATUS_REG1, int_ctrl[STATUS_REG1]);
		if (retval < 0)
			return retval;

		retval = tavarua_write_register(radio,
					STATUS_REG2, int_ctrl[STATUS_REG2]);
		if (retval < 0)
			return retval;

		retval = tavarua_write_register(radio,
					STATUS_REG3, int_ctrl[STATUS_REG3]);
		if (retval < 0)
			return retval;
	}

	radio->lp_mode = 0;
	/* tavarua_handle_interrupts force reads all the interrupt status
	*  registers and it is not valid for MBA 2.1
	*/
	if ((radio->chipID != MARIMBA_2_1) && (radio->chipID != BAHAMA_1_0)
		&& (radio->chipID != BAHAMA_2_0))
		tavarua_handle_interrupts(radio);

	return retval;

}

/*==============================================================
FUNCTION:  tavarua_disable_interrupts
==============================================================*/
/**
  This function disables interrupts.

  @param radio: structure pointer passed by client.

  @return => 0 if successful.
  @return < 0 if failure.
*/
static int tavarua_disable_interrupts(struct tavarua_device *radio)
{
	unsigned char lpm_buf[XFR_REG_NUM];
	int retval;
	if (radio->lp_mode)
		return 0;
	FMDBG("%s\n", __func__);
	/* In Low power mode, disable all the interrupts that are not being
		 waited by the Application */
	lpm_buf[STATUS_REG1] = TUNE | SEARCH | SCANNEXT;
	lpm_buf[STATUS_REG2] = 0x00;
	lpm_buf[STATUS_REG3] = TRANSFER;
	/* use xfr for interrupt setup */
	wait_timeout = 100;
	if (radio->chipID == MARIMBA_2_1 || radio->chipID == BAHAMA_1_0
		|| radio->chipID == BAHAMA_2_0)
		retval = sync_write_xfr(radio, INT_CTRL, lpm_buf);
	/* use register write to setup interrupts */
	else
		retval = tavarua_write_registers(radio, STATUS_REG1, lpm_buf,
							ARRAY_SIZE(lpm_buf));

	/*INT_CTL writes may fail with TIME_OUT as all the
	interrupts have been disabled
	*/
	if (retval > -1 || retval == -ETIME) {
		radio->lp_mode = 1;
		/*Consider timeout as a valid case here*/
		retval = 0;
	}
	wait_timeout = WAIT_TIMEOUT;
	return retval;

}

/*==============================================================
FUNCTION:  tavarua_start
==============================================================*/
/**
  Starts/enables the device (FM radio).

  @param radio: structure pointer passed by client.
  @param state: FM radio state (receiver/transmitter/off/reset).

  @return On success 0 is returned, else error code.
*/
static int tavarua_start(struct tavarua_device *radio,
				enum radio_state_t state)
{

	int retval;
	FMDBG("%s <%d>\n", __func__, state);
	/* set geographic region */
	radio->region_params.region = TAVARUA_REGION_US;

	/* set radio mode */
	retval = tavarua_write_register(radio, RDCTRL, state);
	if (retval < 0)
		return retval;
	/* wait for radio to init */
	msleep(RADIO_INIT_TIME);
	/* enable interrupts */
	tavarua_setup_interrupts(radio, state);
	/* default region is US */
	radio->region_params.band_low = US_LOW_BAND * FREQ_MUL;
	radio->region_params.band_high = US_HIGH_BAND * FREQ_MUL;

	return 0;
}

/*==============================================================
FUNCTION:  tavarua_suspend
==============================================================*/
/**
  Save state and stop all devices in system.

  @param pdev: platform device to be suspended.
  @param state: Power state to put each device in.

  @return On success 0 is returned, else error code.
*/
static int tavarua_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct tavarua_device *radio = platform_get_drvdata(pdev);
	int retval;
	int users = 0;
	printk(KERN_INFO DRIVER_NAME "%s: radio suspend\n\n", __func__);
	if (radio) {
		mutex_lock(&radio->lock);
		users = radio->users;
		mutex_unlock(&radio->lock);
		if (users) {
			retval = tavarua_disable_interrupts(radio);
			if (retval < 0) {
				printk(KERN_INFO DRIVER_NAME
					"tavarua_suspend error %d\n", retval);
				return -EIO;
			}
		}
	}
	return 0;
}

/*==============================================================
FUNCTION:  tavarua_resume
==============================================================*/
/**
  Restore state of each device in system.

  @param pdev: platform device to be resumed.

  @return On success 0 is returned, else error code.
*/
static int tavarua_resume(struct platform_device *pdev)
{

	struct tavarua_device *radio = platform_get_drvdata(pdev);
	int retval;
	int users = 0;
	printk(KERN_INFO DRIVER_NAME "%s: radio resume\n\n", __func__);
	if (radio) {
		mutex_lock(&radio->lock);
		users = radio->users;
		mutex_unlock(&radio->lock);

		if (users) {
			retval = tavarua_setup_interrupts(radio,
			(radio->registers[RDCTRL] & 0x03));
			if (retval < 0) {
				printk(KERN_INFO DRIVER_NAME "Error in \
					tavarua_resume %d\n", retval);
				return -EIO;
			}
		}
	}
	return 0;
}

/*==============================================================
FUNCTION:  tavarua_set_audio_path
==============================================================*/
/**
  This function will configure the audio path to and from the
  FM core.

  This interface is expected to be called from the multimedia
  driver's thread.  This interface should only be called when
  the FM hardware is enabled.  If the FM hardware is not
  currently enabled, this interface will return an error.

  @param digital_on: Digital audio from the FM core should be enabled/disbled.
  @param analog_on: Analog audio from the FM core should be enabled/disbled.

  @return On success 0 is returned, else error code.
*/
int tavarua_set_audio_path(int digital_on, int analog_on)
{
	struct tavarua_device *radio = private_data;
	int rx_on = radio->registers[RDCTRL] & FM_RECV;
	if (!radio)
		return -ENOMEM;
	/* RX */
	FMDBG("%s: digital: %d analog: %d\n", __func__, digital_on, analog_on);
	SET_REG_FIELD(radio->registers[AUDIOCTRL],
		((rx_on && analog_on) ? 1 : 0),
		AUDIORX_ANALOG_OFFSET,
		AUDIORX_ANALOG_MASK);
	SET_REG_FIELD(radio->registers[AUDIOCTRL],
		((rx_on && digital_on) ? 1 : 0),
		AUDIORX_DIGITAL_OFFSET,
		AUDIORX_DIGITAL_MASK);
	SET_REG_FIELD(radio->registers[AUDIOCTRL],
		(rx_on ? 0 : 1),
		AUDIOTX_OFFSET,
		AUDIOTX_MASK);
	SET_REG_FIELD(radio->registers[AUDIOCTRL],
		(0),
		I2SCTRL_OFFSET,
		I2SCTRL_MASK);
	FMDBG("%s: %x\n", __func__, radio->registers[AUDIOCTRL]);
	return tavarua_write_register(radio, AUDIOCTRL,
					radio->registers[AUDIOCTRL]);

}

/*==============================================================
FUNCTION:  tavarua_probe
==============================================================*/
/**
  Once called this functions initiates, allocates resources and registers video
  tuner device with the v4l2 framework.

  NOTE:
  probe() should verify that the specified device hardware
  actually exists; sometimes platform setup code can't be sure.  The probing
  can use device resources, including clocks, and device platform_data.

  @param pdev: platform device to be probed.

  @return On success 0 is returned, else error code.
	-ENOMEM in low memory cases
*/
static int  __init tavarua_probe(struct platform_device *pdev)
{

	struct marimba_fm_platform_data *tavarua_pdata;
	struct tavarua_device *radio;
	int retval;
	int i;
	FMDBG("%s: probe called\n", __func__);
	/* private data allocation */
	radio = kzalloc(sizeof(struct tavarua_device), GFP_KERNEL);
	if (!radio) {
		retval = -ENOMEM;
	goto err_initial;
	}

	radio->marimba = platform_get_drvdata(pdev);
	tavarua_pdata = pdev->dev.platform_data;
	radio->pdata = tavarua_pdata;
	radio->dev = &pdev->dev;
	platform_set_drvdata(pdev, radio);

	/* video device allocation */
	radio->videodev = video_device_alloc();
	if (!radio->videodev)
		goto err_radio;

	/* initial configuration */
	memcpy(radio->videodev, &tavarua_viddev_template,
	  sizeof(tavarua_viddev_template));

	/*allocate internal buffers for decoded rds and event buffer*/
	for (i = 0; i < TAVARUA_BUF_MAX; i++) {
		int kfifo_alloc_rc=0;
		spin_lock_init(&radio->buf_lock[i]);

		if (i == TAVARUA_BUF_RAW_RDS)
			kfifo_alloc_rc = kfifo_alloc(&radio->data_buf[i], 
				rds_buf*3, GFP_KERNEL);
		else
			kfifo_alloc_rc = kfifo_alloc(&radio->data_buf[i], 
				STD_BUF_SIZE, GFP_KERNEL);

		if (kfifo_alloc_rc!=0) {
			printk(KERN_ERR "%s: failed allocating buffers %d\n",
				__func__, kfifo_alloc_rc);
			goto err_bufs;
		}
	}
	/* init xfr status */
	radio->users = 0;
	radio->xfr_in_progress = 0;
	radio->xfr_bytes_left = 0;
	for (i = 0; i < TAVARUA_XFR_MAX; i++)
		radio->pending_xfrs[i] = 0;

	/* init transmit data */
	radio->tx_mode = TAVARUA_TX_RT;
	/* init search params */
	radio->srch_params.srch_pty = 0;
	radio->srch_params.srch_pi = 0;
	radio->srch_params.preset_num = 0;
	radio->srch_params.get_list = 0;
	/* radio initializes to low power mode */
	radio->lp_mode = 1;
	radio->handle_irq = 1;
	/* init lock */
	mutex_init(&radio->lock);
	/* init completion flags */
	init_completion(&radio->sync_xfr_start);
	init_completion(&radio->sync_req_done);
	radio->tune_req = 0;
	/* initialize wait queue for event read */
	init_waitqueue_head(&radio->event_queue);
	/* initialize wait queue for raw rds read */
	init_waitqueue_head(&radio->read_queue);

	video_set_drvdata(radio->videodev, radio);
    /*Start the worker thread for event handling and register read_int_stat
	as worker function*/
	INIT_DELAYED_WORK(&radio->work, read_int_stat);

	/* register video device */
	if (video_register_device(radio->videodev, VFL_TYPE_RADIO, radio_nr)) {
		printk(KERN_WARNING DRIVER_NAME
				": Could not register video device\n");
		goto err_all;
	}
	private_data = radio;
	return 0;

err_all:
	video_device_release(radio->videodev);
err_bufs:
	for (; i > -1; i--)
		kfifo_free(&radio->data_buf[i]);
err_radio:
	kfree(radio);
err_initial:
	return retval;
}

/*==============================================================
FUNCTION:  tavarua_remove
==============================================================*/
/**
  Removes the device.

  @param pdev: platform device to be removed.

  @return On success 0 is returned, else error code.
*/
static int __devexit tavarua_remove(struct platform_device *pdev)
{
	int i;
	struct tavarua_device *radio = platform_get_drvdata(pdev);

	/* disable irq */
	tavarua_disable_irq(radio);

	video_unregister_device(radio->videodev);

	/* free internal buffers */
	for (i = 0; i < TAVARUA_BUF_MAX; i++)
		kfifo_free(&radio->data_buf[i]);

	/* free state struct */
	kfree(radio);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

/*
 Platform drivers follow the standard driver model convention, where
 discovery/enumeration is handled outside the drivers, and drivers
 provide probe() and remove() methods.  They support power management
 and shutdown notifications using the standard conventions.
*/
static struct platform_driver tavarua_driver = {
	.driver = {
		.owner  = THIS_MODULE,
		.name   = "marimba_fm",
	},
	.remove = __devexit_p(tavarua_remove),
	.suspend = tavarua_suspend,
	.resume = tavarua_resume,
}; /* platform device we're adding */


/*************************************************************************
 * Module Interface
 ************************************************************************/

/*==============================================================
FUNCTION:  radio_module_init
==============================================================*/
/**
  Module entry - add a platform-level device.

  @return Returns zero if the driver registered and bound to a device, else
  returns a negative error code when the driver not registered.
*/
static int __init radio_module_init(void)
{
	printk(KERN_INFO DRIVER_DESC ", Version " DRIVER_VERSION "\n");
	return platform_driver_probe(&tavarua_driver, tavarua_probe);
}

/*==============================================================
FUNCTION:  radio_module_exit
==============================================================*/
/**
  Module exit - removes a platform-level device.

  NOTE:
  Note that this function will also release all memory- and port-based
  resources owned by the device (dev->resource).

  @return none.
*/
static void __exit radio_module_exit(void)
{
  platform_driver_unregister(&tavarua_driver);
}

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);

module_init(radio_module_init);
module_exit(radio_module_exit);

