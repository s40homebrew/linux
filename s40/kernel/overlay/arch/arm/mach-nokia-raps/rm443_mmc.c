// SPDX-License-Identifier: GPL-2.0-only
/*
 * RM-443 memory-card controller, PIO implementation.
 *
 * MCUSW F02E75C6/F02E7916 establish the OMAP-style register/command layout;
 * F02E7640 routes IRQ139, GPIO46 is active-high card presence. Registers
 * occupy 32-bit slots but the FIFO and response fragments contain 16 bits.
 * Protocol reference: TI SPRU765A and Linux drivers/mmc/host/omap.c.
 *
 * Pad mux remains as set by S40. The recovered clock selector and CON.POW
 * sequence reinitialize the controller/card; other power domains remain
 * unchanged. No firmware calls, firmware addresses or DMA remain.
 */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>

static bool poll;
module_param(poll, bool, 0444);
MODULE_PARM_DESC(poll, "Poll MMC status without enabling its interrupt (diagnostic)");
static bool trace;
module_param(trace, bool, 0444);
MODULE_PARM_DESC(trace, "Log MMC request boundaries and first block-read stages (diagnostic)");

#define MMC_CMD         0x00
#define MMC_ARGL        0x04
#define MMC_ARGH        0x08
#define MMC_CON         0x0c
#define MMC_STAT        0x10
#define MMC_IE          0x14
#define MMC_CTO         0x18
#define MMC_DTO         0x1c
#define MMC_DATA        0x20
#define MMC_BLEN        0x24
#define MMC_NBLK        0x28
#define MMC_BUF         0x2c
#define MMC_SDIO        0x34
#define MMC_RSP0        0x40
#define MMC_REV         0x60
#define MMC_SYSC        0x64
#define MMC_SYSS        0x68

#define STAT_EOC        BIT(0)
#define STAT_END_DATA   BIT(3)
#define STAT_END_BUSY   BIT(4)
#define STAT_DATA_TOUT  BIT(5)
#define STAT_DATA_CRC   BIT(6)
#define STAT_CMD_TOUT   BIT(7)
#define STAT_CMD_CRC    BIT(8)
#define STAT_AF         BIT(10)
#define STAT_AE         BIT(11)
#define STAT_CARD_ERR   BIT(14)
#define STAT_CMD_ERRORS (STAT_CMD_TOUT | STAT_CMD_CRC)
#define STAT_DATA_ERRORS (STAT_DATA_TOUT | STAT_DATA_CRC)
#define CON_ENABLE      BIT(11)
#define CON_WIDTH4      BIT(15)
#define RM443_MAX_REQ   (32 * 1024)

struct rm443_mmc {
	struct mmc_host *mmc;
	void __iomem *io;
	void __iomem *detect;
	void __iomem *clock;
	void __iomem *semaphore;
	struct completion event;
	struct work_struct work;
	struct mmc_request *mrq;
	u8 *buffer;
	u32 parent_hz;
	u32 clock_hz;
	u16 con;
	u8 power_mode;
	unsigned long power_on_after;
	int ios_error;
	bool read_traced, trace_read;
};

static u16 rm443_read(struct rm443_mmc *host, unsigned int reg)
{
	return readl(host->io + reg);
}

static void rm443_write(struct rm443_mmc *host, unsigned int reg, u16 value)
{
	writel(value, host->io + reg);
}

static int rm443_mmc_get_cd(struct mmc_host *mmc)
{
	struct rm443_mmc *host = mmc_priv(mmc);

	return !!(readl(host->detect) & BIT(14));
}

static irqreturn_t rm443_mmc_irq(int irq, void *arg)
{
	struct rm443_mmc *host = arg;

	if (!(rm443_read(host, MMC_STAT) & rm443_read(host, MMC_IE)))
		return IRQ_NONE;
	/* Leave W1C status for the worker; masking avoids a level IRQ storm. */
	rm443_write(host, MMC_IE, 0);
	complete(&host->event);
	return IRQ_HANDLED;
}

/* All waits are bounded and sleep with the CPU/watchdogs free to run. */
static int rm443_wait(struct rm443_mmc *host, u16 mask,
		      unsigned long deadline)
{
	int status;
	long remaining;
	unsigned int spins = 0;

	if (poll)
		rm443_write(host, MMC_IE, 0);
	for (;;) {
		if (!rm443_mmc_get_cd(host->mmc))
			return -ENOMEDIUM;
		status = rm443_read(host, MMC_STAT);
		if (status & mask)
			return status;
		remaining = deadline - jiffies;
		if (remaining <= 0)
			return -ETIMEDOUT;
		if (poll) {
			/* At 400 kHz a halfword arrives in 40 us. With HZ=100
			 * and no high-resolution timers, sleeping for every word
			 * would take a whole tick. Poll for at most 200 us with
			 * IRQs enabled, then sleep if the controller is still idle. */
			if (++spins < 100)
				udelay(2);
			else {
				usleep_range(20, 40);
				spins = 0;
			}
			continue;
		}
		reinit_completion(&host->event);
		rm443_write(host, MMC_IE, mask);
		/* Check after enabling: a completion preceding the IRQ is valid. */
		if (!(rm443_read(host, MMC_STAT) & mask))
			wait_for_completion_timeout(&host->event,
				min_t(long, remaining, msecs_to_jiffies(100)));
		rm443_write(host, MMC_IE, 0);
	}
}

static int rm443_busy(struct rm443_mmc *host, unsigned int timeout_ms)
{
	int status = rm443_wait(host, STAT_END_BUSY | STAT_DATA_ERRORS,
		jiffies + msecs_to_jiffies(clamp(timeout_ms, 1U, 60000U)));

	if (status < 0)
		return status;
	if (status & STAT_DATA_TOUT)
		return -ETIMEDOUT;
	if (status & STAT_DATA_CRC)
		return -EILSEQ;
	rm443_write(host, MMC_STAT, STAT_END_BUSY);
	return 0;
}

static unsigned int rm443_data_timeout_ms(struct rm443_mmc *host,
					 struct mmc_data *data)
{
	/* Use one-bit wire time even on a four-bit bus. TX can be serviced
	 * while command completion is pending, so that phase needs the same
	 * transfer allowance as the data phase, especially at identification
	 * clock rates. Include the card's access/programming allowance. */
	u64 clocks = (u64)data->blksz * data->blocks * 8 + data->timeout_clks;
	u64 timeout_ms = DIV_ROUND_UP_ULL(clocks * 1000, host->clock_hz ?: 1);

	timeout_ms += DIV_ROUND_UP_ULL(data->timeout_ns, 1000000) + 1000;
	return min_t(u64, timeout_ms, 60000);
}

static void rm443_write_word(struct rm443_mmc *host, unsigned int *done,
			     unsigned int length)
{
	u16 word = host->buffer[(*done)++];

	if (*done < length)
		word |= (u16)host->buffer[(*done)++] << 8;
	/* Discard an earlier block's busy-end before the final word can
	 * complete the final block. */
	if (*done == length)
		rm443_write(host, MMC_STAT, STAT_END_BUSY);
	rm443_write(host, MMC_DATA, word);
}

static int rm443_command(struct rm443_mmc *host, struct mmc_command *cmd,
			 bool reading, unsigned int *written)
{
	u16 encoding = cmd->opcode, response = 0, type;
	u16 mask = STAT_EOC | STAT_CMD_TOUT | STAT_CARD_ERR;
	unsigned int length = written ? host->mrq->data->blksz *
					 host->mrq->data->blocks : 0;
	unsigned long deadline;
	int status, i, error = 0;

	if (cmd->flags & MMC_RSP_136)
		response = 2;
	else if (cmd->flags & MMC_RSP_PRESENT)
		response = cmd->flags & MMC_RSP_CRC ? 1 : 3;
	/* R6/R7 share Linux flags with R1, but have different status fields. */
	if (mmc_cmd_type(cmd) == MMC_CMD_BCR) {
		if (cmd->opcode == SD_SEND_IF_COND)
			response = 4;
		else if (cmd->opcode == SD_SEND_RELATIVE_ADDR)
			response = 6;
	}
	switch (mmc_cmd_type(cmd)) {
	case MMC_CMD_BC: type = 0; break;
	case MMC_CMD_BCR: type = 1; break;
	case MMC_CMD_ADTC: type = 3; break;
	default: type = 2; break;
	}
	encoding |= response << 8 | type << 12;
	if (cmd->flags & MMC_RSP_BUSY)
		encoding |= BIT(11);
	if (reading)
		encoding |= BIT(15);
	/* This removable slot supports SD only; ODTO stays push-pull (zero). */
	rm443_write(host, MMC_IE, 0);
	rm443_write(host, MMC_STAT, 0xffff);
	rm443_write(host, MMC_CTO, 0xfd);
	rm443_write(host, MMC_ARGL, cmd->arg);
	rm443_write(host, MMC_ARGH, cmd->arg >> 16);
	rm443_read(host, MMC_CON); /* OMAP-style posted-write/start ordering. */
	rm443_write(host, MMC_CMD, encoding);
	if (cmd->flags & MMC_RSP_CRC)
		mask |= STAT_CMD_CRC;
	if (written)
		mask |= STAT_DATA_ERRORS;
	deadline = jiffies + msecs_to_jiffies(written ?
			rm443_data_timeout_ms(host, host->mrq->data) : 500);
	for (;;) {
		bool feed = written && *written < length;

		/* The write FIFO requests data before command completion.
		 * Waiting for EOC with an empty FIFO can stop the card clock.
		 * OMAP's IRQ driver also services AE while the command is pending
		 * (SPRU765A, figures 12 and 20). Keep the transferred offset so
		 * the following data phase never repeats these bytes. */
		status = rm443_wait(host, mask | (feed ? STAT_AE : 0), deadline);
		if (status < 0 || (status & mask &
				 (STAT_CMD_ERRORS | STAT_DATA_ERRORS | STAT_CARD_ERR)))
			break;
		if (feed && (status & STAT_AE)) {
			rm443_write(host, MMC_STAT, STAT_AE);
			rm443_write_word(host, written, length);
		}
		/* OMAP can assert an early EOC together with AE on a write.
		 * Let the FIFO advance before accepting that completion. */
		if ((status & STAT_EOC) && !(feed && (status & STAT_AE)))
			break;
		cond_resched();
	}
	if (status < 0)
		error = status;
	else if (status & (STAT_CMD_TOUT | (written ? STAT_DATA_TOUT : 0)))
		error = -ETIMEDOUT;
	else if (((status & STAT_CMD_CRC) && (cmd->flags & MMC_RSP_CRC)) ||
		 (written && (status & STAT_DATA_CRC)))
		error = -EILSEQ;
	else if (status & STAT_CARD_ERR)
		error = -EIO;
	if ((!error || (status >= 0 && (status & STAT_CARD_ERR))) &&
	    (cmd->flags & MMC_RSP_PRESENT)) {
		for (i = 0; i < (cmd->flags & MMC_RSP_136 ? 4 : 1); i++)
			cmd->resp[i] = rm443_read(host, MMC_RSP0 + 24 - i * 8) |
				(u32)rm443_read(host, MMC_RSP0 + 28 - i * 8) << 16;
	}
	/* EOC is suppressed on CARD_ERR; retain the response for diagnosis. */
	if (error && written)
		dev_err_ratelimited(mmc_dev(host->mmc),
			"write command CMD%u failed %d: STAT=%04x sent=%u/%u response=%08x CON=%04x\n",
			cmd->opcode, error, rm443_read(host, MMC_STAT),
			*written, length, cmd->resp[0], rm443_read(host, MMC_CON));
	rm443_write(host, MMC_STAT, STAT_EOC | STAT_CMD_ERRORS | STAT_CARD_ERR);
	if (!error && (cmd->flags & MMC_RSP_BUSY))
		error = rm443_busy(host, cmd->busy_timeout ?: 1000);
	cmd->error = error;
	return error;
}

static int rm443_data(struct rm443_mmc *host, struct mmc_data *data,
		      unsigned int done)
{
	unsigned int length = data->blksz * data->blocks;
	bool reading = data->flags & MMC_DATA_READ;
	u16 ready = reading ? STAT_AF : STAT_AE;
	unsigned int timeout_ms = rm443_data_timeout_ms(host, data);
	unsigned long deadline;
	int status, error = 0;

	deadline = jiffies + msecs_to_jiffies(timeout_ms);
	if (host->trace_read)
		dev_info(mmc_dev(host->mmc), "first read: data start, bytes=%u timeout=%u ms\n",
			 length, timeout_ms);
	while (done < length) {
		u16 word;

		status = rm443_wait(host, ready | STAT_DATA_ERRORS | STAT_CARD_ERR,
				    deadline);
		if (status < 0) {
			error = status;
			break;
		}
		if (status & (STAT_DATA_ERRORS | STAT_CARD_ERR)) {
			error = status & STAT_DATA_TOUT ? -ETIMEDOUT :
				status & STAT_DATA_CRC ? -EILSEQ : -EIO;
			break;
		}
		/* AF/AE are W1C and rearm after the programmed FIFO access count.
		 * Acknowledge before access so a newly ready word is not lost. */
		rm443_write(host, MMC_STAT, ready);
		if (reading) {
			word = rm443_read(host, MMC_DATA);
			host->buffer[done++] = word;
			if (done < length)
				host->buffer[done++] = word >> 8;
		} else
			rm443_write_word(host, &done, length);
		if (!(done & 0xff))
			cond_resched();
	}
	if (!error) {
		if (host->trace_read)
			dev_info(mmc_dev(host->mmc), "first read: FIFO complete, bytes=%u STAT=%04x\n",
				 done, rm443_read(host, MMC_STAT));
		status = rm443_wait(host, STAT_END_DATA | STAT_DATA_ERRORS, deadline);
		if (status < 0)
			error = status;
		else if (status & STAT_DATA_ERRORS)
			error = status & STAT_DATA_TOUT ? -ETIMEDOUT : -EILSEQ;
	}
	/* Writes are not complete until programming/busy has finished. */
	if (!error && !reading)
		error = rm443_busy(host, timeout_ms);
	if (!error && reading) {
		if (host->trace_read)
			dev_info(mmc_dev(host->mmc), "first read: copying %u bytes to request\n", length);
		if (sg_copy_from_buffer(data->sg, data->sg_len, host->buffer, length) != length)
			error = -EINVAL;
	}
	data->bytes_xfered = error ? 0 : length;
	data->error = error;
	if (error)
		dev_err_ratelimited(mmc_dev(host->mmc),
			"CMD%u %s data failed %d: STAT=%04x bytes=%u/%u blocks=%u blksz=%u CON=%04x\n",
			host->mrq->cmd->opcode, reading ? "read" : "write", error,
			rm443_read(host, MMC_STAT), done, length, data->blocks, data->blksz,
			rm443_read(host, MMC_CON));
	return error;
}

static void rm443_recover(struct rm443_mmc *host)
{
	u32 status;

	/* INAB+ADTC aborts the transfer and resets FIFO without dropping POW.
	 * Clearing CON.POW here would power-cycle the card behind MMC core. */
	rm443_write(host, MMC_IE, 0);
	rm443_write(host, MMC_BUF, 0);
	rm443_write(host, MMC_STAT, 0xffff);
	rm443_write(host, MMC_CMD, (3 << 12) | BIT(7));
	if (readl_poll_timeout(host->io + MMC_STAT, status, status & STAT_EOC,
			       10, 5000))
		dev_warn_ratelimited(mmc_dev(host->mmc), "controller abort timed out\n");
	rm443_write(host, MMC_STAT, 0xffff);
}

static void rm443_mmc_work(struct work_struct *work)
{
	struct rm443_mmc *host = container_of(work, struct rm443_mmc, work);
	struct mmc_request *mrq = host->mrq;
	struct mmc_data *data = mrq->data;
	unsigned int length = data ? data->blksz * data->blocks : 0;
	unsigned int written = 0;
	int error = host->ios_error;

	host->trace_read = trace && !host->read_traced && data &&
		(data->flags & MMC_DATA_READ) &&
		(mrq->cmd->opcode == MMC_READ_SINGLE_BLOCK ||
		 mrq->cmd->opcode == MMC_READ_MULTIPLE_BLOCK);
	if (host->trace_read)
		host->read_traced = true;
	if (trace)
		dev_info(mmc_dev(host->mmc),
			 "request start: CMD%u arg=%08x blocks=%u blksz=%u flags=%x STAT=%04x\n",
			 mrq->cmd->opcode, mrq->cmd->arg, data ? data->blocks : 0,
			 data ? data->blksz : 0, data ? data->flags : 0,
			 rm443_read(host, MMC_STAT));
	if (!rm443_mmc_get_cd(host->mmc))
		error = -ENOMEDIUM;
	if (!error && data && (!data->blksz || data->blksz > 2048 ||
	    !data->blocks || data->blocks > 64 || length > RM443_MAX_REQ))
		error = -EINVAL;
	if (!error && data && (data->flags & MMC_DATA_WRITE) &&
	    sg_copy_to_buffer(data->sg, data->sg_len, host->buffer, length) != length)
		error = -EINVAL;
	if (error) {
		mrq->cmd->error = error;
		goto done;
	}
	if (mrq->sbc && rm443_command(host, mrq->sbc, false, NULL)) {
		rm443_recover(host);
		goto done;
	}
	/* One halfword readiness thresholds; both DMA request bits stay clear. */
	rm443_write(host, MMC_BUF, 0);
	rm443_write(host, MMC_BLEN, data ? data->blksz - 1 : 0);
	rm443_write(host, MMC_NBLK, data ? data->blocks - 1 : 0);
	/* Extended DTO multiplier, bounded independently by the software wait. */
	rm443_write(host, MMC_SDIO, BIT(5));
	rm443_write(host, MMC_DTO, 0xffff);
	error = rm443_command(host, mrq->cmd, data && (data->flags & MMC_DATA_READ),
			       data && (data->flags & MMC_DATA_WRITE) ? &written : NULL);
	if (host->trace_read)
		dev_info(mmc_dev(host->mmc), "first read: command complete error=%d STAT=%04x\n",
			 error, rm443_read(host, MMC_STAT));
	if (!error && data)
		error = rm443_data(host, data, written);
	if (host->trace_read)
		dev_info(mmc_dev(host->mmc), "first read: data complete error=%d bytes=%u\n",
			 error, data->bytes_xfered);
	if (error)
		rm443_recover(host);
	/* Always terminate an issued multi-block stream, including error paths. */
	if (mrq->stop && rm443_mmc_get_cd(host->mmc) &&
	    rm443_command(host, mrq->stop, false, NULL))
		rm443_recover(host);
done:
	if (trace)
		dev_info(mmc_dev(host->mmc),
			 "request done: CMD%u arg=%08x blocks=%u blksz=%u error=%d sbc=%d data=%d stop=%d bytes=%u STAT=%04x\n",
			 mrq->cmd->opcode, mrq->cmd->arg, data ? data->blocks : 0,
			 data ? data->blksz : 0, mrq->cmd->error,
			 mrq->sbc ? mrq->sbc->error : 0, data ? data->error : 0,
			 mrq->stop ? mrq->stop->error : 0, data ? data->bytes_xfered : 0,
			 rm443_read(host, MMC_STAT));
	rm443_write(host, MMC_IE, 0);
	rm443_write(host, MMC_STAT, 0xffff);
	host->mrq = NULL;
	mmc_request_done(host->mmc, mrq);
}

static void rm443_mmc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct rm443_mmc *host = mmc_priv(mmc);

	host->mrq = mrq;
	schedule_work(&host->work);
}

static void rm443_mmc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct rm443_mmc *host = mmc_priv(mmc);
	u32 divider = 1023, status;
	bool init_stream = ios->power_mode == MMC_POWER_ON &&
		host->power_mode != MMC_POWER_ON;
	bool power_up = ios->power_mode != MMC_POWER_OFF &&
		host->power_mode == MMC_POWER_OFF;
	long remaining;

	host->ios_error = 0;
	host->power_mode = ios->power_mode;
	if (ios->clock)
		divider = clamp(DIV_ROUND_UP(host->parent_hz, ios->clock), 1U, 1023U);
	host->clock_hz = ios->clock ? host->parent_hz / divider : 0;
	mmc->actual_clock = host->clock_hz;
	host->con = divider;
	if (ios->bus_width == MMC_BUS_WIDTH_4)
		host->con |= CON_WIDTH4;
	if (ios->power_mode != MMC_POWER_OFF)
		host->con |= CON_ENABLE;
	/* F02E8678 waits 200 ms after off, then 20 ms after enabling POW.
	 * Schematic LP3929 EN is MMC_CMT10/GPIO7. Its pad mux is inherited. */
	if (power_up) {
		remaining = host->power_on_after - jiffies;
		if (remaining > 0)
			msleep(jiffies_to_msecs(remaining) + 1);
	}
	rm443_write(host, MMC_CON, host->con);
	rm443_write(host, MMC_CON, host->con);
	if (power_up)
		msleep(20);
	if (ios->power_mode == MMC_POWER_OFF)
		host->power_on_after = jiffies + msecs_to_jiffies(200);
	if (init_stream) {
		rm443_write(host, MMC_IE, 0);
		rm443_write(host, MMC_STAT, 0xffff);
		rm443_write(host, MMC_CMD, BIT(7));
		host->ios_error = readl_poll_timeout(host->io + MMC_STAT, status,
						   status & STAT_EOC, 10, 2000);
		rm443_write(host, MMC_STAT, STAT_EOC);
	}
}

static const struct mmc_host_ops rm443_mmc_ops = {
	.request = rm443_mmc_request,
	.set_ios = rm443_mmc_set_ios,
	.get_cd = rm443_mmc_get_cd,
};

static void rm443_mmc_select_clock(struct rm443_mmc *host, unsigned int source)
{
	u32 value = readl(host->clock + 0x10) & ~GENMASK(11, 10);

	/* F02E7362 clears this field before selecting the new clock source. */
	writel(value, host->clock + 0x10);
	writel(value | source << 10, host->clock + 0x10);
}

static int rm443_mmc_reset(struct rm443_mmc *host)
{
	u32 status;
	unsigned int attempt;
	int error = -ETIMEDOUT;

	rm443_write(host, MMC_IE, 0);
	for (attempt = 0; attempt < 2; ++attempt) {
		unsigned int source = attempt ? 1 : 3;
		unsigned int divider = attempt ? 241 : 316;

		rm443_write(host, MMC_CON, 0);
		msleep(200);
		/* Establish the clock before requesting reset as well as after:
		 * a pending reset needs a live functional clock to synchronize.
		 * Source3 is S40's reset choice. If it cannot complete after
		 * takeover, retry on the source1 clock used for identification.
		 * Both settings come from RM-443 firmware; no shared PLL is
		 * retuned. A readback alone is never treated as reset completion.
		 */
		rm443_mmc_select_clock(host, source);
		rm443_write(host, MMC_SYSC, BIT(1));
		rm443_write(host, MMC_CON, CON_ENABLE);
		rm443_mmc_select_clock(host, source);
		/* Match F02E7362's separate clear/set divider writes. */
		rm443_write(host, MMC_CON, rm443_read(host, MMC_CON) & ~1023);
		rm443_write(host, MMC_CON, CON_ENABLE | divider);
		error = readl_poll_timeout(host->io + MMC_SYSS, status, status & 1,
					   10, 100000);
		if (!error) {
			dev_info(mmc_dev(host->mmc), "reset complete: source%u divider%u REV=%04x\n",
				 source, divider, rm443_read(host, MMC_REV));
			msleep(20);
			break;
		}
		dev_warn(mmc_dev(host->mmc),
			 "reset source%u timed out: CON=%04x SYSC=%04x SYSS=%08x\n",
			 source, rm443_read(host, MMC_CON), rm443_read(host, MMC_SYSC), status);
	}
	if (error)
		dev_err(mmc_dev(host->mmc),
			"controller reset failed: REV=%04x clock=%08x gates=%08x control=%08x PLL0=%08x PLL1=%08x PLL2=%08x\n",
			rm443_read(host, MMC_REV), readl(host->clock + 0x10),
			readl(host->clock + 0xd0), readl(host->clock + 0x80),
			readl(host->clock + 0x120), readl(host->clock + 0x140),
			readl(host->clock + 0x180));
	/* Give MMC core a powered-off slot. Its next power-up observes the
	 * same 200 ms off interval and uses the known 96 MHz source1 clock.
	 */
	rm443_write(host, MMC_CON, 0);
	host->power_on_after = jiffies + msecs_to_jiffies(200);
	rm443_mmc_select_clock(host, 1);
	return error;
}

static int rm443_mmc_clock_enable(struct rm443_mmc *host)
{
	u32 value;
	unsigned int tries;
	int error;

	/* Native resource group3 enables the shared PLL under semaphore2,
	 * followed by MMC's gate (resource5). Reading the semaphore acquires
	 * it: release exactly once, and only after a successful acquisition. */
	for (tries = 0; tries < 100000; tries++) {
		if ((s32)readl(host->semaphore) > 0)
			break;
		cpu_relax();
	}
	if (tries == 100000)
		return -EBUSY;
	writel(0x8000, host->clock + 0x88);
	error = readl_poll_timeout(host->clock + 0x180, value, value & 1,
				   10, 10000);
	writel(U32_MAX, host->semaphore);
	if (error)
		return error;
	writel(0x20, host->clock + 0xd4);
	return 0;
}

static int rm443_mmc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mmc_host *mmc;
	struct rm443_mmc *host;
	u32 width = 4, max_hz = 12500000;
	int irq, error;

	mmc = devm_mmc_alloc_host(dev, sizeof(*host));
	if (!mmc)
		return -ENOMEM;
	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->io = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->io))
		return PTR_ERR(host->io);
	/* Read-only shared input register: keypad owns neighbouring GPIOs. */
	host->detect = devm_ioremap(dev, 0x20031d88, 4);
	if (!host->detect)
		return -ENOMEM;
	host->clock = devm_ioremap(dev, 0x20032c00, 0x200);
	if (!host->clock)
		return -ENOMEM;
	host->semaphore = devm_ioremap(dev, 0x2004020c, 4);
	if (!host->semaphore)
		return -ENOMEM;
	host->buffer = devm_kmalloc(dev, RM443_MAX_REQ, GFP_KERNEL);
	if (!host->buffer)
		return -ENOMEM;
	error = of_property_read_u32(dev->of_node, "clock-frequency", &host->parent_hz);
	if (error || !host->parent_hz || host->parent_hz > 400000000)
		return dev_err_probe(dev, -EINVAL, "missing/invalid parent clock-frequency\n");
	of_property_read_u32(dev->of_node, "max-frequency", &max_hz);
	of_property_read_u32(dev->of_node, "bus-width", &width);
	if ((width != 1 && width != 4) || !max_hz || max_hz > 25000000)
		return -EINVAL;
	init_completion(&host->event);
	INIT_WORK(&host->work, rm443_mmc_work);
	error = rm443_mmc_clock_enable(host);
	if (error)
		return dev_err_probe(dev, error, "MMC clock enable timed out\n");
	error = rm443_mmc_reset(host);
	if (error)
		return error;
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	if (!poll) {
		error = devm_request_irq(dev, irq, rm443_mmc_irq, 0, dev_name(dev), host);
		if (error)
			return error;
	}
	mmc->ops = &rm443_mmc_ops;
	mmc->f_min = DIV_ROUND_UP(host->parent_hz, 1023);
	mmc->f_max = min(max_hz, host->parent_hz);
	/* Schematic N3200/LP3929 supplies the card at 2.85 V. */
	mmc->ocr_avail = MMC_VDD_28_29;
	mmc->caps = MMC_CAP_NEEDS_POLL | MMC_CAP_WAIT_WHILE_BUSY;
	if (width == 4)
		mmc->caps |= MMC_CAP_4_BIT_DATA;
	mmc->caps2 = MMC_CAP2_NO_SDIO | MMC_CAP2_NO_MMC | MMC_CAP2_NO_WRITE_PROTECT;
	mmc->max_busy_timeout = 60000;
	mmc->max_blk_size = 2048;
	mmc->max_blk_count = 64;
	mmc->max_req_size = RM443_MAX_REQ;
	mmc->max_seg_size = RM443_MAX_REQ;
	mmc->max_segs = 128;
	platform_set_drvdata(pdev, host);
	error = mmc_add_host(mmc);
	if (error)
		return error;
	dev_info(dev, "RAPS PIO SD host, IRQ%d, GPIO46 detect, selector1 parent %u Hz, max %u Hz; inherited pad mux\n",
		 irq, host->parent_hz, mmc->f_max);
	if (poll)
		dev_info(dev, "diagnostic polling: MMC interrupt remains masked\n");
	return 0;
}

static void rm443_mmc_remove(struct platform_device *pdev)
{
	struct rm443_mmc *host = platform_get_drvdata(pdev);

	mmc_remove_host(host->mmc);
	cancel_work_sync(&host->work);
	rm443_write(host, MMC_IE, 0);
	rm443_write(host, MMC_CON, 0);
}

static const struct of_device_id rm443_mmc_match[] = {
	{ .compatible = "nokia,rm443-mmc" },
	{}
};

static struct platform_driver rm443_mmc_driver = {
	.probe = rm443_mmc_probe,
	.remove = rm443_mmc_remove,
	.driver = { .name = "rm443-mmc", .of_match_table = rm443_mmc_match },
};
builtin_platform_driver(rm443_mmc_driver);
