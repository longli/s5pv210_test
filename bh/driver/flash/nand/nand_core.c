#include <io.h>
#include <errno.h>
#include <delay.h>
#include <malloc.h>
#include <assert.h>
#include <string.h>
#include <bitops.h>
#include <flash/flash.h>
#include <flash/nand.h>

#define BBT_PAGE_MASK	0xffffff3f

struct jffs2_clean_mark {
	__u16 magic;
	__u16 node_type;
	__u32 total_len;
};

extern const struct nand_desc g_nand_chip_desc[];
extern const struct nand_vendor_name g_nand_vendor_id[];

static struct nand_oob_layout g_oob8_layout = {
	.ecc_code_len = 3,
	.ecc_pos = {0, 1, 2},
	.free_region = {{3, 2}, {6, 2}}
};

// small page layout
static struct nand_oob_layout g_oob16_layout = {
	.ecc_code_len = 6,
	.ecc_pos = {0, 1, 2, 3, 6, 7},
	.free_region = {{8, 8}}
};

// large page layout
static struct nand_oob_layout g_oob64_layout = {
	.ecc_code_len = 24,
	.ecc_pos = {
	   40, 41, 42, 43, 44, 45, 46, 47,
	   48, 49, 50, 51, 52, 53, 54, 55,
	   56, 57, 58, 59, 60, 61, 62, 63
	},
	.free_region = {{2, 38}}
};

static int  nand_do_write_oob(struct nand_chip *nand, __u32 to, struct oob_opt *opt);

static void nand_wait_ready(struct nand_chip *nand);

static void nand_release_chip(struct nand_chip *nand)
{
	struct nand_ctrl *nfc = nand->master;

	nfc->select_chip(nand, false);
	nfc->state = FL_READY;
}

static __u8 nand_read_u8(struct nand_ctrl *nfc)
{
	return readb(nfc->data_reg);
}

// fixme:
static __u8 nand_read_u16(struct nand_ctrl *nfc)
{
	return (__u8) cpu_to_le16(readw(nfc->data_reg));
}

// fixme:
static __u16 nand_read_u16ex(struct nand_ctrl *nfc)
{
	return readw(nfc->data_reg);
}

static void nand_chipsel(struct nand_chip *nand, bool chip_select)
{
	struct nand_ctrl *nfc = nand->master;

	if (false == chip_select)
		nfc->cmd_ctrl(nand, NAND_CMMD_NONE, 0 | NAND_CTRL_CHANGE);
}

static void nand_write_buff(struct nand_ctrl *nfc, const __u8 *buff, int len)
{
	int i;

	for (i = 0; i < len; i++)
		writeb(nfc->data_reg, buff[i]);
}

static void nand_read_buff(struct nand_ctrl *nfc, __u8 *buff, int len)
{
	int i;

	for (i = 0; i < len; i++)
		buff[i] = readb(nfc->data_reg);
}

static int nand_verify_buff(struct nand_ctrl *nfc, const __u8 *buff, int len)
{
	int i;

	for (i = 0; i < len; i++)
		if (buff[i] != readb(nfc->data_reg))
			return -EFAULT;
	return 0;
}

static void nand_write_buff16(struct nand_ctrl *nfc, const __u8 *buff, int len)
{
	int i;
	__u16 *p;

	p = (__u16 *)buff;
	len >>= 1;

	for (i = 0; i < len; i++)
		writew(nfc->data_reg, p[i]);
}

static void nand_read_buff16(struct nand_ctrl *nfc, __u8 *buff, int len)
{
	int i;

	__u16 *p = (__u16 *) buff;
	len >>= 1;

	for (i = 0; i < len; i++)
		p[i] = readw(nfc->data_reg);
}

static int nand_verify_buff16(struct nand_ctrl *nfc, const __u8 *buff, int len)
{
	int i;

	__u16 *p = (__u16 *) buff;
	len >>= 1;

	for (i = 0; i < len; i++)
		if (p[i] != readw(nfc->data_reg))
			return -EFAULT;

	return 0;
}

static int nand_blk_bad(struct nand_chip *nand, __u32 ofs, int getchip)
{
	__u16 bad;
	int page, res = 0;
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	page = (int)(ofs >> flash->write_shift) & nand->page_num_mask;

	if (getchip) {
		// chipnr = (int)(ofs >> flash->chip_shift);
		nfc->select_chip(nand, true);
	}

	if (nand->flags & NAND_BUSWIDTH_16) {
		nfc->command(nand, NAND_CMMD_READOOB,
			nand->bad_blk_oob_pos & 0xFE, page);

		bad = cpu_to_le16(nfc->read_word(nfc));
		if (nand->bad_blk_oob_pos & 0x1)
			bad >>= 8;
		if ((bad & 0xFF) != 0xFF)
			res = 1;
	} else {
		nfc->command(nand, NAND_CMMD_READOOB, nand->bad_blk_oob_pos, page);
		if (nfc->read_byte(nfc) != 0xFF)
			res = 1;
	}

	if (getchip)
		nand_release_chip(nand);

	return res;
}

static int nand_mark_blk_bad(struct nand_chip *nand, __u32 ofs)
{

	__u8 buff[2] = { 0, 0 };
	int block, ret;
	struct flash_chip *flash = NAND_TO_FLASH(nand);

	block = (int)(ofs >> nand->bbt_erase_shift);
	if (nand->bbt)
		nand->bbt[block >> 2] |= 0x01 << ((block & 0x03) << 1);

	if (nand->flags & NAND_USE_FLASH_BBT)
		ret = nand_update_bbt(nand, ofs);
	else {
		ofs += flash->oob_size;
		nand->opt.data_len = nand->opt.oob_len = 2;
		nand->opt.data_buff = NULL;
		nand->opt.oob_buff = buff;
		nand->opt.oob_off = nand->bad_blk_oob_pos & ~0x01;

		ret = nand_do_write_oob(nand, ofs, &nand->opt);
		nand_release_chip(nand);
	}

	if (!ret)
		flash->eccstat.badblocks++;

	return ret;
}

static int nand_check_wp(struct nand_chip *nand)
{
	__u8 status;
	struct nand_ctrl *nfc = nand->master;

	nfc->command(nand, NAND_CMMD_STATUS, -1, -1);

	status = nfc->read_byte(nfc);

	return (status & NAND_STATUS_WP) ? 0 : 1;
}

static int nand_check_blk_bad(struct nand_chip *nand, __u32 ofs, int getchip)
{
	struct nand_ctrl *nfc = nand->master;

	if (!nand->bbt)
		return nfc->block_bad(nand, ofs, getchip);

	return nand_is_bad_bbt(nand, ofs);
}

static void nand_wait_ready(struct nand_chip *nand)
{
	int to;
	struct nand_ctrl *nfc = nand->master;

	for (to = 0; to < 10; to++) {
		if (nfc->flash_ready(nand))
			break;

		udelay(nfc->chip_delay);
	}
}

static void nand_command_small(struct nand_chip *nand,
							__u32 command, int col, int row)
{
	int ctrl = NAND_CTRL_CLE | NAND_CTRL_CHANGE;
	struct flash_chip    *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	if (command == NAND_CMMD_SEQIN) {
		int read_cmd;

		if (col >= flash->write_size) {
			col -= flash->write_size;
			read_cmd = NAND_CMMD_READOOB;
		} else if (col < 256)
			read_cmd = NAND_CMMD_READ0;
		else {
			col -= 256;
			read_cmd = NAND_CMMD_READ1;
		}

		nfc->cmd_ctrl(nand, read_cmd, ctrl);

		ctrl &= ~NAND_CTRL_CHANGE;
	}

	nfc->cmd_ctrl(nand, command, ctrl);

	ctrl = NAND_CTRL_ALE | NAND_CTRL_CHANGE;

	if (col != -1) {
		if (nand->flags & NAND_BUSWIDTH_16)
			col >>= 1;

		nfc->cmd_ctrl(nand, col, ctrl);

		ctrl &= ~NAND_CTRL_CHANGE;
	}

	if (row != -1) {
		nfc->cmd_ctrl(nand, row, ctrl);

		ctrl &= ~NAND_CTRL_CHANGE;
		nfc->cmd_ctrl(nand, row >> 8, ctrl);

		if (flash->chip_size > (1 << 25))
			nfc->cmd_ctrl(nand, row >> 16, ctrl);
	}

	nfc->cmd_ctrl(nand, NAND_CMMD_NONE, NAND_NCE | NAND_CTRL_CHANGE);

	switch (command) {
	case NAND_CMMD_PAGEPROG:
	case NAND_CMMD_ERASE1:
	case NAND_CMMD_ERASE2:
	case NAND_CMMD_SEQIN:
	case NAND_CMMD_STATUS:
		return; // break or return ?

	case NAND_CMMD_RESET:
		if (nfc->flash_ready)
			break;

		udelay(nfc->chip_delay);

		nfc->cmd_ctrl(nand, NAND_CMMD_STATUS, NAND_CTRL_CLE | NAND_CTRL_CHANGE);
		nfc->cmd_ctrl(nand, NAND_CMMD_NONE, NAND_NCE | NAND_CTRL_CHANGE);
		while (!(nfc->read_byte(nfc) & NAND_STATUS_READY));

		return;

	default:
		if (!nfc->flash_ready) {
			udelay(nfc->chip_delay);
			return;
		}

		break;
	}

	// fixme
	udelay(nfc->chip_delay);

	nand_wait_ready(nand);
}

static void nand_command_large(struct nand_chip *nand,
				__u32 command, int col, int row)
{
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	if (command == NAND_CMMD_READOOB) {
		col += flash->write_size;
		command = NAND_CMMD_READ0;
	}

	nfc->cmd_ctrl(nand,	command & 0xFF,
			NAND_NCE | NAND_CLE | NAND_CTRL_CHANGE);

	if (col != -1 || row != -1) {
		int ctrl = NAND_CTRL_CHANGE | NAND_NCE | NAND_ALE;

		if (col != -1) {
			if (nand->flags & NAND_BUSWIDTH_16)
				col >>= 1;

			nfc->cmd_ctrl(nand, col, ctrl);
			ctrl &= ~NAND_CTRL_CHANGE;
			nfc->cmd_ctrl(nand, col >> 8, ctrl);
		}

		if (row != -1) {
			nfc->cmd_ctrl(nand, row, ctrl);
			nfc->cmd_ctrl(nand, row >> 8, NAND_NCE | NAND_ALE);

			if (flash->chip_size > (1 << 27))
				nfc->cmd_ctrl(nand, row >> 16, NAND_NCE | NAND_ALE);
		}
	}

	nfc->cmd_ctrl(nand, NAND_CMMD_NONE, NAND_NCE | NAND_CTRL_CHANGE);

	switch (command) {
	case NAND_CMMD_CACHEDPROG:
	case NAND_CMMD_PAGEPROG:
	case NAND_CMMD_ERASE1:
	case NAND_CMMD_ERASE2:
	case NAND_CMMD_SEQIN:
	case NAND_CMMD_RNDIN:
	case NAND_CMMD_STATUS:
	case NAND_CMMD_DEPLETE1:
		return;

	case NAND_CMMD_STATUS_ERROR:
	case NAND_CMMD_STATUS_ERROR0:
	case NAND_CMMD_STATUS_ERROR1:
	case NAND_CMMD_STATUS_ERROR2:
	case NAND_CMMD_STATUS_ERROR3:
		udelay(nfc->chip_delay);
		return;

	case NAND_CMMD_RESET:
		if (nfc->flash_ready)
			break;

		udelay(nfc->chip_delay);

		nfc->cmd_ctrl(nand, NAND_CMMD_STATUS, NAND_NCE | NAND_CLE | NAND_CTRL_CHANGE);
		nfc->cmd_ctrl(nand, NAND_CMMD_NONE, NAND_NCE | NAND_CTRL_CHANGE);
		while (!(nfc->read_byte(nfc) & NAND_STATUS_READY)); // fixme

		return;

	case NAND_CMMD_RNDOUT:
		nfc->cmd_ctrl(nand,
				NAND_CMMD_RNDOUTSTART, NAND_NCE | NAND_CLE | NAND_CTRL_CHANGE);
		nfc->cmd_ctrl(nand, NAND_CMMD_NONE, NAND_NCE | NAND_CTRL_CHANGE);
		return;

	case NAND_CMMD_READ0:
		nfc->cmd_ctrl(nand, NAND_CMMD_READSTART, NAND_NCE | NAND_CLE | NAND_CTRL_CHANGE);
		nfc->cmd_ctrl(nand, NAND_CMMD_NONE, NAND_NCE | NAND_CTRL_CHANGE);

	default:
		if (!nfc->flash_ready) {
			udelay(nfc->chip_delay);
			return;
		}

		break;
	}

	// ndelay(100);
	ndelay(nfc->chip_delay);

	nand_wait_ready(nand);
}

// fixme
static int nand_wait(struct nand_chip *nand)
{
	int status, state, to;
	struct nand_ctrl *nfc = nand->master;

	// fixme: tWB
	// ndelay(100);
	ndelay(nfc->chip_delay);

	state = nfc->state;
	if ((state == FL_ERASING) && (nand->flags & NAND_IS_AND))
		nfc->command(nand, NAND_CMMD_STATUS_MULTI, -1, -1);
	else
		nfc->command(nand, NAND_CMMD_STATUS, -1, -1);

	for (to = 0; to < 10; to++) {
		if (nfc->flash_ready) {
			if (nfc->flash_ready(nand))
				break;
		} else {
			if (nfc->read_byte(nfc) & NAND_STATUS_READY)
				break;
		}
	}

	status = (int)nfc->read_byte(nfc);

	return status;
}

static int nand_read_page_raw(struct nand_chip *nand, __u8 *buff)
{
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	nfc->read_buff(nfc, buff, flash->write_size);
	nfc->read_buff(nfc, nand->oob_buf, flash->oob_size);

	return 0;
}

static int nand_read_page_swecc(struct nand_chip *nand, __u8 *buff)
{
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;
	int i;
	__u8 *p;

	__u8 *pEccCopDescurr  = nand->buffers->ecccalc;
	__u8 *pEccCodesSaved = nand->buffers->ecccode;
	__u32 *ecc_pos     = nfc->curr_oob_layout->ecc_pos;

	nfc->read_page_raw(nand, buff);

	i = 0;
	p = buff;

	while (p < buff + flash->write_size) {
		nand_calculate_ecc(nand, p, pEccCopDescurr + i);

		i += SOFT_ECC_CODE_NUM;
		p += SOFT_ECC_DATA_LEN;
	}

	while (--i >= 0)
		pEccCodesSaved[i] = nand->oob_buf[ecc_pos[i]];

	i = 0;
	p = buff;

	while (p < buff + flash->write_size) {
		int stat;

		stat = nand_correct_data(nand, p, &pEccCodesSaved[i], pEccCopDescurr + i);

		if (stat < 0)
			flash->eccstat.ecc_failed_count++;
		else
			flash->eccstat.ecc_correct_count += stat;

		i += SOFT_ECC_CODE_NUM;
		p += SOFT_ECC_DATA_LEN;
	}

	return 0;
}

static void nand_write_page_swecc(struct nand_chip *nand, const __u8 *buff)
{
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;
	int i;
	const __u8 *p;
	__u8 *ecc_code  = nand->buffers->ecccalc;
	__u32 *ecc_pos = nfc->curr_oob_layout->ecc_pos;

	i = 0;
	p = buff;

	while (p < buff + flash->write_size) {
		nand_calculate_ecc(nand, p, ecc_code + i);

		i += SOFT_ECC_CODE_NUM;
		p += SOFT_ECC_DATA_LEN;
	}

	while(--i >= 0)
		nand->oob_buf[ecc_pos[i]] = ecc_code[i];

	nfc->write_page_raw(nand, buff);
}

static void nand_write_page_hwecc(struct nand_chip *nand, const __u8 *buff)
{
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	const __u8 *p;
	int i;

	int ecc_data_len = nfc->ecc_data_len;
	int ecc_code_len = nfc->ecc_code_len;

	__u8  *ecc_code = nand->buffers->ecccalc;
	__u32 *ecc_pos  = nfc->curr_oob_layout->ecc_pos;

	i = 0;
	p = buff;

	while (p < buff + flash->write_size) {
		nfc->ecc_enable(nand, NAND_ECC_WRITE);
		nfc->write_buff(nfc, p, ecc_data_len);
		nfc->ecc_generate(nand, p, ecc_code + i);

		i += ecc_code_len;
		p += ecc_data_len;
	}

	while(--i >= 0)
		nand->oob_buf[ecc_pos[i]] = ecc_code[i];

	nfc->write_buff(nfc, nand->oob_buf, flash->oob_size);
}

static int nand_read_page_hwecc(struct nand_chip *nand, __u8 *buff)
{
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	int i;
	__u8 *p;

	int ecc_data_len = nfc->ecc_data_len;
	int ecc_code_len = nfc->ecc_code_len;

	__u8 *pEccCodesSaved = nand->buffers->ecccode;
	__u8 *pEccCopDescurr = nand->buffers->ecccalc;
	__u32 *ecc_pos     = nfc->curr_oob_layout->ecc_pos;

	i = 0;
	p = buff;

	while (p < buff + flash->write_size) {
		nfc->ecc_enable(nand, NAND_ECC_READ);
		nfc->read_buff(nfc, p, ecc_data_len);
		nfc->ecc_generate(nand, p, pEccCopDescurr + i);

		i += ecc_code_len;
		p += ecc_data_len;
	}

	nfc->read_buff(nfc, nand->oob_buf, flash->oob_size);

	while(--i >= 0)
		pEccCodesSaved[i] = nand->oob_buf[ecc_pos[i]];

	i = 0;
	p = buff;

	while (p < buff + flash->write_size) {
		int stat;

		stat = nfc->ecc_correct(nand, p, &pEccCodesSaved[i], pEccCopDescurr + i);

		if (stat < 0)
			flash->eccstat.ecc_failed_count++;
		else
			flash->eccstat.ecc_correct_count += stat;

		i += ecc_code_len;
		p += ecc_data_len;
	}

	return 0;
}

static __u8 *nand_copy_oob(struct nand_chip *nand,
				__u8 *oob_buf, struct oob_opt *opt, __u32 len)
{
	struct nand_ctrl *nfc = nand->master;

	switch(opt->op_mode) {
	case FLASH_OOB_PLACE:
	case FLASH_OOB_RAW:
		memcpy(oob_buf, nand->oob_buf + opt->oob_off, len);
		return oob_buf + len;

	case FLASH_OOB_AUTO:
	{
		struct oob_free_region *free = nfc->curr_oob_layout->free_region;
		__u32 boffs = 0, roffs = opt->oob_off;
		__u32 bytes = 0;

		for(; free->nOfLen && len; free++, len -= bytes) {
			if (roffs) {
				if (roffs >= free->nOfLen) {
					roffs -= free->nOfLen;
					continue;
				}
				boffs = free->nOfOffset + roffs;
				bytes = min(len, free->nOfLen - roffs);
				roffs = 0;
			} else {
				bytes = min(len, free->nOfLen);
				boffs = free->nOfOffset;
			}

			memcpy(oob_buf, nand->oob_buf + boffs, bytes);
			oob_buf += bytes;
		}

		return oob_buf;
	}

	default:
		BUG();
	}
	return NULL;
}

static int nand_read_by_opt(struct nand_chip *nand, __u32 from, struct oob_opt *opt)
{
	int ret = 0;
	__u32 readlen;
	__u32 oobreadlen;
	__u8 *oob_buf, *buff;
	int page, real_page;
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;
	struct ecc_stats stats;

	stats = flash->eccstat;

	// chipnr = from >> flash->chip_shift;
	nfc->select_chip(nand, true);

	real_page = from >> flash->write_shift;
	page = real_page & nand->page_num_mask;

	buff = opt->data_buff;
	oob_buf = opt->oob_buff;

	readlen = 0;
	oobreadlen = 0;

	// TODO: support cache read
	while (readlen < opt->data_len) {
		nfc->command(nand, NAND_CMMD_READ0, 0x00, page);

		switch (opt->op_mode) {
		case FLASH_OOB_RAW:
			ret = nfc->read_page_raw(nand, buff);
			if (ret < 0)
				goto L1;

			buff = nand_copy_oob(nand,
						buff + flash->write_size, opt, flash->oob_size);
			readlen += flash->write_size + flash->oob_size;

			break;

		case FLASH_OOB_AUTO:
		case FLASH_OOB_PLACE:
			ret = nfc->read_page(nand, buff);
			if (ret < 0)
				goto L1;

			buff += flash->write_size;
			readlen += flash->write_size;

			if (oob_buf) {
				oob_buf = nand_copy_oob(nand, oob_buf, opt, opt->oob_len);
				oobreadlen += opt->oob_len; // error! fix nand_copy_oob()
			}

			break;

		default:
			BUG();
		}

		if (flash->callback_func && flash->callback_args) {
			flash->callback_args->page_index  = page;
			flash->callback_args->block_index = page >> (flash->erase_shift - flash->write_shift);

			flash->callback_func(flash, flash->callback_args);
		}

		// here is the right place ?
		if (!nfc->flash_ready)
			udelay(nfc->chip_delay);
		else
			nand_wait_ready(nand);

		real_page++;
		page = real_page & nand->page_num_mask;
	}

L1:
	opt->ret_len = readlen;

	if (oob_buf)
		opt->oob_ret_len = oobreadlen;

	if (ret)
		return ret;

	if (flash->eccstat.ecc_failed_count - stats.ecc_failed_count)
		return -EBADMSG;

	return flash->eccstat.ecc_correct_count - stats.ecc_correct_count ? -EUCLEAN : 0;
}

static int nand_read(struct nand_chip *nand,
				__u32 from, __u32 len, __u32 *retlen, __u8 *buff)
{
	int ret;
	struct flash_chip *flash = NAND_TO_FLASH(nand);

	if (from + len > flash->chip_size) {
		DPRINT("%s(): read range beyond flash chip size!\n", __func__);
		return -EINVAL;
	}

	if (!len)
		return 0;

	nand->opt.data_len = len;
	nand->opt.data_buff = buff;
	nand->opt.oob_buff = NULL;

	ret = nand_read_by_opt(nand, from, &nand->opt);

	*retlen = nand->opt.ret_len;

	nand_release_chip(nand);

	return ret;
}

static int nand_read_oob_std(struct nand_chip *nand, int page, int sndcmd)
{
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	if (sndcmd) {
		nfc->command(nand, NAND_CMMD_READOOB, 0, page);
		sndcmd = 0;
	}

	nfc->read_buff(nfc, nand->oob_buf, flash->oob_size);

	return sndcmd; // fixme
}

static int nand_write_oob_std(struct nand_chip *nand, int page)
{
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	int status = 0;
	const __u8 *buff = nand->oob_buf;
	int length = flash->oob_size;

	nfc->command(nand, NAND_CMMD_SEQIN, flash->write_size, page);
	nfc->write_buff(nfc, buff, length);

	nfc->command(nand, NAND_CMMD_PAGEPROG, -1, -1);

	status = nfc->wait_func(nand);

	return status & NAND_STATUS_FAIL ? -EIO : 0;
}

static int nand_do_read_oob(struct nand_chip *nand, __u32 from, struct oob_opt *opt)
{
	int page, real_page, chipnr, sndcmd = 1;
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;
	int blkcheck = (1 << (nand->phy_erase_shift - flash->write_shift)) - 1;
	int readlen = opt->oob_len;
	int len;
	__u8 *buff = opt->oob_buff;

	if (opt->op_mode == FLASH_OOB_AUTO)
		len = nfc->curr_oob_layout->free_oob_sum;
	else
		len = flash->oob_size;

	if ((opt->oob_off >= len)) {
		DPRINT("%s(): Attempt to start read outside oob\n", __func__);
		return -EINVAL;
	}

	// fixme
	if ((from >= flash->chip_size ||
		opt->oob_off + readlen > ((flash->chip_size >> flash->write_shift) -
		(from >> flash->write_shift)) * len))
	{
		DPRINT("%s(): Attempt read beyond end of device\n", __func__);
		return -EINVAL;
	}

	chipnr = (int)(from >> flash->chip_shift);
	nfc->select_chip(nand, true);

	real_page = (int)(from >> flash->write_shift);
	page = real_page & nand->page_num_mask;

	while (1) {
		sndcmd = nfc->read_oob(nand, page, sndcmd);

		len = min(len, readlen);
		buff = nand_copy_oob(nand, buff, opt, len);

		if (!(nand->flags & NAND_NO_READRDY)) {
			if (!nfc->flash_ready)
				udelay(nfc->chip_delay);
			else
				nand_wait_ready(nand);
		}

		readlen -= len;
		if (!readlen)
			break;

		if (flash->callback_func && flash->callback_args) {
			flash->callback_args->page_index  = page;
			flash->callback_args->block_index = page >> (flash->erase_shift - flash->write_shift);

			flash->callback_func(flash, flash->callback_args);
		}

		real_page++;

		page = real_page & nand->page_num_mask;

		if (!page) {
			chipnr++;
			nfc->select_chip(nand, false);
			nfc->select_chip(nand, true);
		}

		if (!NAND_CANAUTOINCR(nand) || !(page & blkcheck))
			sndcmd = 1;
	}

	opt->oob_ret_len = opt->oob_len;

	return 0;
}

static int nand_read_oob(struct nand_chip *nand, __u32 from, struct oob_opt *opt)
{

	int ret = -ENOTSUPP;
	struct flash_chip *flash = NAND_TO_FLASH(nand);

	opt->ret_len = 0;

	if (opt->data_buff && (from + opt->data_len) > flash->chip_size) {
		DPRINT("%s(): Attempt read beyond end of device\n", __func__);
		return -EINVAL;
	}

	switch(opt->op_mode) {
	case FLASH_OOB_PLACE:
	case FLASH_OOB_AUTO:
	case FLASH_OOB_RAW:
		break;

	default:
		goto out;
	}

	if (NULL == opt->data_buff)
		ret = nand_do_read_oob(nand, from, opt);
	else
		ret = nand_read_by_opt(nand, from, opt);

out:
	nand_release_chip(nand);
	return ret;
}

static void nand_write_page_raw(struct nand_chip *nand, const __u8 *buff)
{
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	nfc->write_buff(nfc, buff, flash->write_size);
	nfc->write_buff(nfc, nand->oob_buf, flash->oob_size);
}

static __u8 *nand_fill_oob(struct nand_chip *nand, __u8 *oob_buf, struct oob_opt *opt)
{
	__u32 len = opt->oob_len;
	struct nand_ctrl *nfc = nand->master;

	switch(opt->op_mode) {
	case FLASH_OOB_PLACE:
	case FLASH_OOB_RAW:
		memcpy(nand->oob_buf + opt->oob_off, oob_buf, len);
		return oob_buf + len;

	case FLASH_OOB_AUTO:
	{
		__u32 boffs = 0, bytes = 0, woffs = opt->oob_off;
		struct oob_free_region *free = nfc->curr_oob_layout->free_region;

		for(; free->nOfLen && len; free++, len -= bytes) {
			if (woffs) {
				if (woffs >= free->nOfLen) {
					woffs -= free->nOfLen;
					continue;
				}

				boffs = free->nOfOffset + woffs;
				bytes = min(len, free->nOfLen - woffs);
				woffs = 0;
			} else {
				bytes = min(len, free->nOfLen);
				boffs = free->nOfOffset;
			}

			memcpy(nand->oob_buf + boffs, oob_buf, bytes);
			oob_buf += bytes;
		}

		return oob_buf;
	}

	default:
		BUG();
	}

	return NULL;
}

static inline bool check_aligned(__u32 align, __u32 val)
{
	return (val & (align - 1)) == 0;
}

static int nand_write_by_opt(struct nand_chip *nand, long to, struct oob_opt *opt)
{
	int status;
	int real_page, page;
	__u32 write_len;
	__u8 *buff, *oob_buf;
	__u32 page_buff_offset;
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	buff = opt->data_buff;
	oob_buf = opt->oob_buff;

	opt->ret_len = 0;
	if (!opt->data_len)
		return 0;

	if (!check_aligned(flash->write_size, to)) {
		printf("%s(): page not aligned! page size = 0x%x, to = 0x%x\n",
			__func__, flash->write_size, to);
		return -EINVAL;
	}

#if 0
	// TODO: check opt->data_len align
	if (FLASH_OOB_RAW != opt->op_mode) {
		if (!check_aligned(flash->write_size, opt->data_len)) {
			printf("%s(): page not aligned!\n", __func__);
			return -EINVAL;
		}
	} else {
		if (opt->data_len % (flash->write_size + flash->oob_size)) {
			printf("%s(): page not aligned!\n", __func__);
			return -EINVAL;
		}
	}
#endif

	nfc->select_chip(nand, true);

	if (nand_check_wp(nand)) {
		BUG();
		return -EIO;
	}

	real_page = to >> flash->write_shift;
	page = real_page & nand->page_num_mask;
	// blockmask = (1 << (nand->phy_erase_shift - flash->write_shift)) - 1;

	page_buff_offset = nand->page_in_buff << flash->write_shift;
	if (to <= page_buff_offset && page_buff_offset < to + opt->data_len)
		nand->page_in_buff = -1;

	if (!oob_buf)
		memset(nand->oob_buf, 0xFF, flash->oob_size);

	write_len = 0;

	while (write_len < opt->data_len) {
		__u8 *curr_buff = buff;

		buff += flash->write_size;
		write_len += flash->write_size;

		nfc->command(nand, NAND_CMMD_SEQIN, 0x00, page);

		switch (opt->op_mode) {
		case FLASH_OOB_RAW:
			buff = nand_fill_oob(nand, buff, opt);
			write_len += flash->oob_size; // or opt.oob_len;

			nfc->write_page_raw(nand, curr_buff);

			break;

		case FLASH_OOB_PLACE:
		case FLASH_OOB_AUTO:
			if (oob_buf)
				oob_buf = nand_fill_oob(nand, oob_buf, opt);

			nfc->write_page(nand, curr_buff);

			break;

		default:
			BUG();
		}

		nfc->command(nand, NAND_CMMD_PAGEPROG, -1, -1);

		status = nfc->wait_func(nand);

		if (status & NAND_STATUS_FAIL) {
			DPRINT("%s(): error @ line %d\n", __func__, __LINE__);
			return -EIO;
		}

		if (flash->callback_func && flash->callback_args) {
			flash->callback_args->page_index = page;
			flash->callback_args->block_index = \
				page >> (flash->erase_shift - flash->write_shift);

			flash->callback_func(flash, flash->callback_args);
		}

		real_page++;
		page = real_page & nand->page_num_mask;
	}

	opt->ret_len = write_len;

	if (oob_buf)
		opt->oob_ret_len = opt->oob_len;

	return 0;
}

static int nand_write(struct nand_chip *nand,
				__u32 to, __u32 len, __u32 *retlen, const __u8 *buff)
{
	int ret;
	struct flash_chip *flash = NAND_TO_FLASH(nand);

	if (to + len > flash->chip_size) {
		printf("%s(): error @ line %d. range error!\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (!len)
		return 0;

	nand->opt.data_len  = len;
	nand->opt.data_buff = (__u8 *)buff;
	nand->opt.oob_buff  = NULL;
	nand->opt.op_mode   = FLASH_OOB_PLACE;

	ret = nand_write_by_opt(nand, to, &nand->opt);
	*retlen = nand->opt.ret_len;

	nand_release_chip(nand);

	return ret;
}

static int nand_do_write_oob(struct nand_chip *nand, __u32 to, struct oob_opt *opt)
{
	int page, status, len;
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	DPRINT("%s(): to = 0x%08x, len = %i\n",
		__func__, to, opt->oob_len);

	if (opt->op_mode == FLASH_OOB_AUTO)
		len = nfc->curr_oob_layout->free_oob_sum;
	else
		len = flash->oob_size;

	if ((opt->oob_off + opt->oob_len) > len) {
		DPRINT("%s(): Attempt to write past end of page\n", __func__);
		return -EINVAL;
	}

	if ((opt->oob_off >= len)) {
		DPRINT("%s(): Attempt to start write outside oob\n", __func__);
		return -EINVAL;
	}

	// fixme
	if ((to >= flash->chip_size ||
		opt->oob_off + opt->oob_len >
			((flash->chip_size >> flash->write_shift) -
			 (to >> flash->write_shift)) * len))
	{
		DPRINT("%s(): Attempt write beyond end of device\n", __func__);
		return -EINVAL;
	}

	// chipnr = (int)(to >> flash->chip_shift);
	nfc->select_chip(nand, true);

	page = (int)(to >> flash->write_shift);

	nfc->command(nand, NAND_CMMD_RESET, -1, -1);

	if (nand_check_wp(nand))
		return -EROFS;

	if (page == nand->page_in_buff)
		nand->page_in_buff = -1;

	memset(nand->oob_buf, 0xFF, flash->oob_size);
	nand_fill_oob(nand, opt->oob_buff, opt);

	status = nfc->write_oob(nand, page & nand->page_num_mask);
	// fixme
	memset(nand->oob_buf, 0xFF, flash->oob_size);

	if (status)
		return status;

	opt->oob_ret_len = opt->oob_len;

	return 0;
}

static int nand_write_oob(struct nand_chip *nand, __u32 to, struct oob_opt *opt)
{
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	int ret;

	opt->ret_len = 0;

	if (opt->data_buff && (to + opt->data_len) > flash->chip_size) {
		printf("%s(): Attempt read beyond end of device\n", __func__);
		return -EINVAL;
	}

	if (!opt->data_buff)
		ret = nand_do_write_oob(nand, to, opt);
	else
		ret = nand_write_by_opt(nand, to, opt);

	nand_release_chip(nand);

	return ret;
}

static void erase_one_block(struct nand_chip *nand, int page)
{
	struct nand_ctrl *nfc = nand->master;

	nfc->command(nand, NAND_CMMD_ERASE1, -1, page);
	nfc->command(nand, NAND_CMMD_ERASE2, -1, -1);
}

static void erase_block_ex(struct nand_chip *nand, int page)
{
	struct nand_ctrl *nfc = nand->master;

	nfc->command(nand, NAND_CMMD_ERASE1, -1, page++);
	nfc->command(nand, NAND_CMMD_ERASE1, -1, page++);
	nfc->command(nand, NAND_CMMD_ERASE1, -1, page++);
	nfc->command(nand, NAND_CMMD_ERASE1, -1, page);
	nfc->command(nand, NAND_CMMD_ERASE2, -1, -1);
}

// fixme: static
int nand_erase(struct nand_chip *nand, struct erase_opt *opt)
{
	int page_index, erase_count;
	int status, nPagesPerBlock, ret, chipnr = nand->bus_idx; // fixme!
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	int rewrite_bbt[NAND_MAX_CHIPS]={0};
	unsigned int bbt_masked_page = 0xffffffff;

	DPRINT("%s(): start = 0x%08x, size = 0x%08x\n",
			__func__, opt->estart, opt->esize);

	// fixme
	if (opt->estart & (flash->erase_size - 1)) {
		printf("%s(): erase start not aligned! (0x%08x)\n", __func__, opt->estart);
		return -EINVAL;
	}

	if (opt->esize & (flash->erase_size - 1)) {
		printf("%s(): erase size not aligned! (0x%08x)\n", __func__, opt->esize);
		return -EINVAL;
	}

	if ((opt->esize + opt->estart) > flash->chip_size) {
		printf("%s(): Data range beyond chip size! (0x%08x > 0x%08x)\n",
			__func__,
			opt->esize + opt->estart,
			flash->chip_size
			);

		return -EINVAL;
	}

	opt->fail_addr = 0xffffffff;

	page_index     = opt->estart >> flash->write_shift;
	nPagesPerBlock = flash->erase_size >> flash->write_shift;

	nfc->select_chip(nand, true);

	if (nand_check_wp(nand)) {
		opt->estate = FLASH_ERASE_FAILED;
		DPRINT("%s(): Flash device is write protected!\n", __func__);
		goto erase_exit;
	}

	if (nand->flags & BBT_AUTO_REFRESH && !flash->bad_allow)
		bbt_masked_page = nand->bbt_td->pages[chipnr] & BBT_PAGE_MASK;

	erase_count = opt->esize;

	opt->estate = FLASH_ERASING;

	while (erase_count) {
		if (!(opt->flags & EDF_ALLOWBB)) {	// fixme!
			if (nand_check_blk_bad(nand, page_index << flash->write_shift, false)) {
				printf("\n%s(): try to erase a bad block at 0x%08x!\n",
					__func__, page_index << flash->write_shift);

				opt->estate = FLASH_ERASE_FAILED;
				goto erase_exit;
			}
		}

		if (page_index <= nand->page_in_buff && nand->page_in_buff < page_index + nPagesPerBlock)
			nand->page_in_buff = -1;

		nfc->erase_block(nand, page_index & nand->page_num_mask);

		status = nfc->wait_func(nand);

		if (status & NAND_STATUS_FAIL) {
			opt->estate = FLASH_ERASE_FAILED;
			opt->fail_addr = page_index << flash->write_shift;

			printf("\n%s(): Failed @ page 0x%08x\n", __func__, page_index);
			// fixme!!
			goto erase_exit;
		}

		if (bbt_masked_page != 0xffffffff && (page_index & BBT_PAGE_MASK) == bbt_masked_page)
			rewrite_bbt[chipnr] = page_index << flash->write_shift;

		if (opt->flags & EDF_JFFS2) {
			// fixme
			static struct jffs2_clean_mark cleanmark = {
				0x1985,
				0x2003
			};

			switch (flash->oob_size) {
			case 16:
				cleanmark.total_len = 8;
				break;

			default: // 64?
				GEN_DBG("oob_size (%d) not supported now!\n", flash->oob_size);
				break;
			};

			nfc->command(nand, NAND_CMMD_SEQIN, nand->parent.write_size + 8, page_index);

			nfc->write_buff(nfc, (__u8 *)&cleanmark, 8); // cleanmark.total_len

			nfc->command(nand, NAND_CMMD_PAGEPROG, -1, -1);

			nfc->wait_func(nand);
		}

		if (flash->callback_func && flash->callback_args) {
			flash->callback_args->page_index  = page_index;
			flash->callback_args->block_index = page_index >> (flash->erase_shift - flash->write_shift);

			flash->callback_func(flash, flash->callback_args);
		}

		erase_count -= flash->erase_size;
		page_index	+= nPagesPerBlock;
	}

	opt->estate = FLASH_ERASE_DONE;

erase_exit:
	ret = opt->estate == FLASH_ERASE_DONE ? 0 : -EIO;

	nand_release_chip(nand);

	if (bbt_masked_page == 0xffffffff || ret)
		return ret;

	for (chipnr = 0; chipnr < nfc->slaves; chipnr++) {
		if (!rewrite_bbt[chipnr])
			continue;

		DPRINT("%s(): Updating bad block table (%d:0x%0x 0x%0x) ... \n",
			__func__, chipnr, rewrite_bbt[chipnr], nand->bbt_td->pages[chipnr]);

		nand_update_bbt(nand, rewrite_bbt[chipnr]);
	}

	return ret;
}

static int nand_blk_is_bad(struct nand_chip *nand, __u32 offs)
{
	struct flash_chip *flash = NAND_TO_FLASH(nand);

	if (offs > flash->chip_size)
		return -EINVAL;

	return nand_check_blk_bad(nand, offs, 1);
}

static int nand_block_mark_bad(struct nand_chip *nand, __u32 ofs)
{
	int ret;
	struct nand_ctrl *nfc = nand->master;

	ret = nand_blk_is_bad(nand, ofs);
	if (ret) {
		if (ret > 0)
			return 0;
		return ret;
	}

	return nfc->block_mark_bad(nand, ofs);
}

static void nand_cmd_ctrl(struct nand_chip *nand, int arg, unsigned int ctrl)
{
	struct nand_ctrl *nfc;

	if (arg == NAND_CMMD_NONE)
		return;

	nfc = nand->master;

	if (ctrl & NAND_CLE)
		writeb(nfc->cmmd_reg, arg);
	else
		writeb(nfc->addr_reg, arg);
}

struct nand_ctrl *nand_ctrl_new(void)
{
	struct nand_ctrl *nfc;

	nfc = zalloc(sizeof(struct nand_ctrl));
	if (NULL == nfc)
		return NULL;

	list_head_init(&nfc->nand_list);

	nfc->chip_delay     = 5;
	nfc->slaves         = 0;
	nfc->max_slaves     = 1;
	nfc->ecc_mode       = NAND_ECC_NONE;

	nfc->cmd_ctrl       = nand_cmd_ctrl;
	nfc->command        = nand_command_small;
	nfc->wait_func      = nand_wait;
	nfc->select_chip    = nand_chipsel;
	nfc->read_byte      = nand_read_u8;
	nfc->read_word      = nand_read_u16ex;  // fixme: remove it
	nfc->block_bad      = nand_blk_bad;
	nfc->block_mark_bad = nand_mark_blk_bad;
	nfc->write_buff     = nand_write_buff;
	nfc->read_buff      = nand_read_buff;
	nfc->verify_buff    = nand_verify_buff;
	nfc->scan_bad_block = nand_scan_bbt;
	//
	nfc->erase_block    = erase_one_block;
	nfc->read_page_raw  = nand_read_page_raw;
	nfc->write_page_raw = nand_write_page_raw;
	nfc->read_oob       = nand_read_oob_std;
	nfc->write_oob      = nand_write_oob_std;
	// SE
	nfc->read_page      = nand_read_page_swecc;
	nfc->write_page     = nand_write_page_swecc;

	return nfc;
}

static int probe_nand_chip(struct nand_chip *nand)
{
// #define BUFF_IDR
	int i;
	struct flash_chip *flash = NAND_TO_FLASH(nand);
	struct nand_ctrl *nfc = nand->master;

	nfc->select_chip(nand, true);

	nfc->command(nand, NAND_CMMD_RESET, -1, -1);
	nfc->command(nand, NAND_CMMD_READID, 0x00, -1);

// fixme!
#ifdef BUFF_IDR
	{
		__u8 nand_id[2];

		nfc->read_buff(nfc, nand_id, 2);
		nand->vendor_id = nand_id[0];
		nand->device_id = nand_id[1];
	}
#else
	nand->vendor_id = nfc->read_byte(nfc);
	nand->device_id = nfc->read_byte(nfc);
#endif

	for (i = 0; g_nand_chip_desc[i].name != NULL; i++) {
		if (nand->device_id == g_nand_chip_desc[i].id) {
			flash->chip_size = g_nand_chip_desc[i].chip_size << 20;
			nand->flags = g_nand_chip_desc[i].flags;
			nand->name = g_nand_chip_desc[i].name;

			//
			if (PAGE_SIZE_AUTODETECT != g_nand_chip_desc[i].write_size) {
				flash->erase_size = g_nand_chip_desc[i].erase_size;
				flash->write_size = g_nand_chip_desc[i].write_size;
				flash->oob_size   = flash->write_size / 32;
			} else {
				__u8 ext_id[2];

#ifdef BUFF_IDR
				nfc->read_buff(nfc, ext_id, 2);
#else
				ext_id[0] = nfc->read_byte(nfc);
				ext_id[1] = nfc->read_byte(nfc);
#endif

				DPRINT("%s(): Extend ID of nand[%d] is 0x%02x (\"%s\")\n",
					__func__, nand->bus_idx, ext_id[1], nand->name);

				flash->write_size = 1024 << (ext_id[1] & 0x3);
				ext_id[1] >>= 2;

				flash->oob_size = (8 << (ext_id[1] & 0x01)) * (flash->write_size >> 9);
				ext_id[1] >>= 2;

				flash->erase_size = (64 * 1024) << (ext_id[1] & 0x03);
				ext_id[1] >>= 2;

				if (ext_id[1] & 0x01)
					nand->flags |= NAND_BUSWIDTH_16;
				else
					nand->flags &= ~NAND_BUSWIDTH_16;
			}

			nfc->select_chip(nand, false);

			nand->flags |= NAND_NO_AUTOINCR;  // fix various modes

			if (nand->flags & NAND_BUSWIDTH_16) {
				nfc->read_byte   = nand_read_u16;
				nfc->write_buff  = nand_write_buff16;
				nfc->read_buff   = nand_read_buff16;
				nfc->verify_buff = nand_verify_buff16;
			}

			return 0;
		}
	}

	nfc->select_chip(nand, false);

	return -ENODEV;
}

static int flash2nand_read(struct flash_chip *flash,
				__u32 from, __u32 len, __u32 *retlen, __u8 *buff)
{
	struct nand_chip *nand = FLASH_TO_NAND(flash);

	return nand_read(nand, from, len, retlen, buff);
}

static int flash2nand_write(struct flash_chip *flash,
				__u32 to, __u32 len, __u32 *retlen, const __u8 *buff)
{
	struct nand_chip *nand = FLASH_TO_NAND(flash);

	return nand_write(nand, to, len, retlen, buff);
}

static int flash2nand_erase(struct flash_chip *flash, struct erase_opt *instr)
{
	struct nand_chip *nand = FLASH_TO_NAND(flash);

	return nand_erase(nand, instr);
}

static int flash2nand_read_oob(struct flash_chip *flash,
				__u32 from, struct oob_opt *opt)
{
	struct nand_chip *nand = FLASH_TO_NAND(flash);

	return nand_read_oob(nand, from, opt);
}

static int flash2nand_write_oob(struct flash_chip *flash,
				__u32 to,	struct oob_opt *opt)
{
	struct nand_chip *nand = FLASH_TO_NAND(flash);

	return nand_write_oob(nand, to, opt);
}

static int flash2nand_block_is_bad(struct flash_chip *flash, __u32 offs)
{
	struct nand_chip *nand = FLASH_TO_NAND(flash);

	return nand_blk_is_bad(nand, offs);
}

static int flash2nand_block_mark_bad(struct flash_chip *flash, __u32 ofs)
{
	struct nand_chip *nand = FLASH_TO_NAND(flash);

	return nand_block_mark_bad(nand, ofs);
}

static int flash2nand_block_scan_bad_block(struct flash_chip *flash)
{
	struct nand_chip *nand = FLASH_TO_NAND(flash);
	struct nand_ctrl  *nfc  = nand->master;

	return nfc->scan_bad_block(nand);
}

static struct nand_chip *new_nand_chip(struct nand_ctrl *master, int bus_id)
{
	struct nand_chip *nand;
	struct flash_chip *flash;

	nand = zalloc(sizeof(struct nand_chip));

	if (NULL == nand)
		return NULL;

	flash = NAND_TO_FLASH(nand);

	// flash->part_tab = (struct partition *)malloc(sizeof(*flash->part_tab) * MAX_FLASH_PARTS);

	nand->master = master;
	nand->bus_idx = bus_id;

	list_add_tail(&nand->nand_node, &master->nand_list);

	flash->type = FLASH_NANDFLASH;
	flash->bad_allow = true;

	flash->read  = flash2nand_read;
	flash->write = flash2nand_write;
	flash->erase = flash2nand_erase;

	flash->read_oob  = flash2nand_read_oob;
	flash->write_oob = flash2nand_write_oob;

	flash->block_is_bad   = flash2nand_block_is_bad;
	flash->block_mark_bad = flash2nand_block_mark_bad;
	flash->scan_bad_block = flash2nand_block_scan_bad_block;

	return nand;
}

static void delete_nand_chip(struct nand_chip *nand)
{
	// struct flash_chip *flash;

	list_del_node(&nand->nand_node);

	// flash = NAND_TO_FLASH(nand);
	// fixme: free all buffer.
	// free(flash->part_tab);

	free(nand);
}

ECC_MODE nand_set_ecc_mode(struct nand_ctrl *nfc, ECC_MODE new_mode)
{
	int i;
	ECC_MODE old_mode;

#if 0
	if (nfc->ecc_mode == new_mode)
		return new_mode;
#endif

	printf("    ECC Mode   = ");

	switch (new_mode) {
	case NAND_ECC_HW:
		if (!nfc->ecc_enable || !nfc->ecc_generate || !nfc->ecc_correct) {
			printf("Driver seems does not support hardware ECC!\n");
			return false;
		}

		nfc->read_page      = nand_read_page_hwecc;
		nfc->write_page     = nand_write_page_hwecc;
		nfc->curr_oob_layout = nfc->hard_oob_layout;

		//nfc->nEccOobBytes = nfc->ecc_code_len;  // fixme
		if (!nfc->hard_oob_layout) {
			printf("Invalid Hardware ECC!\n");
			return -EINVAL;
		}

		printf("Hardware\n");
		break;

	case NAND_ECC_SW:
		nfc->read_page      = nand_read_page_swecc;
		nfc->write_page     = nand_write_page_swecc;
		nfc->curr_oob_layout = nfc->soft_oob_layout;
		if (!nfc->ecc_data_len)
			nfc->ecc_data_len = 256;
		nfc->ecc_code_len = 3;

		if (!nfc->soft_oob_layout) {
			printf("Invalid Software ECC!\n");
			return -EINVAL;
		}

		printf("Software\n");
		break;

	case NAND_ECC_NONE:
		nfc->read_page      = nand_read_page_raw;
		nfc->write_page     = nand_write_page_raw;
		nfc->curr_oob_layout = NULL;

		printf("None\n");
		goto L1; // break;

	default:
		printf("Invalid(%d) !\n", nfc->ecc_mode);
		BUG();

		return NAND_ECC_NONE;
	}

	nfc->curr_oob_layout->free_oob_sum = 0;

	for (i = 0; nfc->curr_oob_layout->free_region[i].nOfLen; i++)
		nfc->curr_oob_layout->free_oob_sum += nfc->curr_oob_layout->free_region[i].nOfLen;

L1:
	old_mode = nfc->ecc_mode;
	nfc->ecc_mode = new_mode;

	return old_mode;
}

int flash_set_ecc_mode(struct flash_chip *flash, ECC_MODE new_mode, ECC_MODE *old_mode)
{
	struct nand_chip *nand = FLASH_TO_NAND(flash);

	*old_mode = nand_set_ecc_mode(nand->master, new_mode);

	return 0;
}

struct nand_chip *nand_probe(struct nand_ctrl *nfc, int bus_idx)
{
	int ret, i;
	struct nand_chip *nand;

	if ((!nfc->cmmd_reg || !nfc->addr_reg) && nfc->cmd_ctrl == nand_cmd_ctrl) {
		printf("error: cmmd_reg or addr_reg is not specified!\n");
		return NULL;
	}

	nand = new_nand_chip(nfc, bus_idx);
	if (NULL == nand)
		return NULL;

	ret = probe_nand_chip(nand);
	if (ret < 0)
		goto not_found;

	// fixme
	for (i = 0; g_nand_vendor_id[i].id; i++) {
		if (g_nand_vendor_id[i].id == nand->vendor_id)
			return nand;
	}

not_found:
	delete_nand_chip(nand);
	return NULL;
}

int nand_register(struct nand_chip *nand)
{
	int i, ret;
	struct flash_chip *flash;
	struct nand_ctrl *nfc;
	char vendor_name[64];
	char chip_size[32], block_size[32], write_size[32];

	nfc = nand->master;

	for (i = 0; g_nand_vendor_id[i].id; i++) {
		if (g_nand_vendor_id[i].id == nand->vendor_id) {
			strcpy(vendor_name, g_nand_vendor_id[i].name);
			break;
		}
	}

	if (0 == g_nand_vendor_id[i].id)
		strcpy(vendor_name, "Unknown vendor");

	flash = NAND_TO_FLASH(nand);

	snprintf(flash->name, sizeof(flash->name), "%s.%d",
		nfc->name, nand->bus_idx /* fixme */);

	val_to_hr_str(flash->chip_size, chip_size);
	val_to_hr_str(flash->erase_size, block_size);
	val_to_hr_str(flash->write_size, write_size);

	printf("NAND[%d] is detected! flash details:\n"
		"    vendor ID  = 0x%02x (%s)\n"
		"    device ID  = 0x%02x (\"%s\")\n"
		"    chip size  = 0x%08x (%s)\n"
		"    block size = 0x%08x (%s)\n"
		"    page size  = 0x%08x (%s)\n"
		"    oob size   = %d\n"
		"    bus width  = %d bits\n",
		nand->bus_idx,
		nand->vendor_id, vendor_name,
		nand->device_id, nand->name,
		flash->chip_size, chip_size,
		flash->erase_size, block_size,
		flash->write_size, write_size,
		flash->oob_size,
		nand->flags & NAND_BUSWIDTH_16 ? 16 : 8
		);

	// fix for name

	flash->chip_shift  = ffs(flash->chip_size) - 1; //fixme: to opt ffs()
	flash->erase_shift = ffs(flash->erase_size) - 1;
	flash->write_shift = ffs(flash->write_size) - 1;

	nand->page_num_mask = (flash->chip_size >> flash->write_shift) - 1;

	nand->bbt_erase_shift = ffs(flash->erase_size) - 1;
	nand->phy_erase_shift = ffs(flash->erase_size) - 1;

	nand->bad_blk_oob_pos = flash->write_size > 512 ? NAND_BBP_LARGE : NAND_BBP_SMALL;

	if ((nand->flags & NAND_4PAGE_ARRAY) && erase_one_block == nfc->erase_block)
		nfc->erase_block = erase_block_ex;

	if (flash->write_size > 512 && nand_command_small == nfc->command)
		nfc->command = nand_command_large;

	if (!(nand->flags & NAND_OWN_BUFFERS)) {
		nand->buffers = malloc(sizeof(*nand->buffers));

		if (!nand->buffers)
			return -ENOMEM;

		nand->oob_buf = nand->buffers->data_buff + flash->write_size;
	}

	switch (flash->oob_size) {
	case 8:
		nfc->soft_oob_layout = &g_oob8_layout;
		break;
	case 16:
		nfc->soft_oob_layout = &g_oob16_layout;
		break;
	case 64:
		nfc->soft_oob_layout = &g_oob64_layout;
		break;
	default:
		printf("No OOB layout defined! OOB Size = %d.\n", flash->oob_size);
		BUG();
	}

	nand_set_ecc_mode(nfc, nfc->ecc_mode);

	nand->page_in_buff = -1;

	flash->bdev.base = 0;
	flash->bdev.size = flash->chip_size;

	ret = flash_register(flash);
	if (ret < 0) {
		printf("%s(): fail to register deivce!\n");
		// fixme: destroy
	}

	if (nand->flags & NAND_SKIP_BBTSCAN)
		return 0;

	printf("Scanning bad blocks:\n");
	ret = nfc->scan_bad_block(nand);
	// fixme
	if (0 == ret)
		printf("done!\n");
	else
		printf("ecc_failed_count > 0!\n");

	nfc->slaves++;

	// printf("Total: %d nand %s detected\n", nfc->slaves, nfc->slaves > 1 ? "chips" : "chip");

	nfc->state = FL_READY;

	return  0;
}

int nand_ctrl_register(struct nand_ctrl *nfc)
{
	int idx, ret;
	struct nand_chip *nand;

	for (idx = 0; idx < nfc->max_slaves; idx++) {
		nand = nand_probe(nfc, idx);
		if (NULL == nand)
			continue;

		ret = nand_register(nand);
		if (ret < 0)
			return ret;
	}

	// printf("Total: %d nand %s detected\n", nfc->slaves, nfc->slaves > 1 ? "chips" : "chip");

	nfc->state = FL_READY;

	return nfc->slaves == 0 ? -ENODEV : nfc->slaves;
}
