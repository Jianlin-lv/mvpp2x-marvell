/*
* ***************************************************************************
* Copyright (C) 2016 Marvell International Ltd.
* ***************************************************************************
* This program is free software: you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the Free
* Software Foundation, either version 2 of the License, or any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
* ***************************************************************************
*/

#include "mv_pp2x.h"

/* Static declaractions */

#ifdef CONFIG_MVPP2_FPGA

static struct pci_device_id fpga_id_table[] = {
	{0x1234, 0x1234},
	{}
};
#endif

/* Number of RXQs used by single port */
static int rxq_number = 1;
/* Number of TXQs used by single port */
static int txq_number = 1;
/*
 * All 4 interfaces use the same global buffer, since only one interface
 * can be enabled at once
 */
static struct buffer_location buffer_loc;


/* Utility/helper methods */
void mv_pp2x_write(struct mv_pp2x *pp2, u32 offset, u32 data)
{
	writel(data, pp2->base + offset);
}

u32 mv_pp2x_read(struct mv_pp2x *pp2, u32 offset)
{
	return readl(pp2->base + offset);
}

/* Get number of physical egress port */
static inline int mv_pp2x_egress_port(struct mv_pp2x_port *port)
{
	return MVPP2_MAX_TCONT + port->id;
}

/* Get number of physical TXQ */
static inline int mv_pp2x_txq_phys(int port, int txq)
{
	return (MVPP2_MAX_TCONT + port) * MVPP2_MAX_TXQ + txq;
}

/* Update parser tcam and sram hw entries */
int mv_pp2x_prs_hw_write(struct mv_pp2x *pp2, struct mv_pp2x_prs_entry *pe)
{
	int i;

	if (pe->index > MVPP2_PRS_TCAM_SRAM_SIZE - 1)
		return -EINVAL;

	/* Clear entry invalidation bit */
	pe->tcam.word[MVPP2_PRS_TCAM_INV_WORD] &= ~MVPP2_PRS_TCAM_INV_MASK;

	/* Write tcam index - indirect access */
	mv_pp2x_write(pp2, MVPP2_PRS_TCAM_IDX_REG, pe->index);
	for (i = 0; i < MVPP2_PRS_TCAM_WORDS; i++)
		mv_pp2x_write(pp2, MVPP2_PRS_TCAM_DATA_REG(i), pe->tcam.word[i]);

	/* Write sram index - indirect access */
	mv_pp2x_write(pp2, MVPP2_PRS_SRAM_IDX_REG, pe->index);
	for (i = 0; i < MVPP2_PRS_SRAM_WORDS; i++)
		mv_pp2x_write(pp2, MVPP2_PRS_SRAM_DATA_REG(i), pe->sram.word[i]);

	return 0;
}

/* Read tcam entry from hw */
int mv_pp2x_prs_hw_read(struct mv_pp2x *pp2, struct mv_pp2x_prs_entry *pe)
{
	int i;

	if (pe->index > MVPP2_PRS_TCAM_SRAM_SIZE - 1)
		return -EINVAL;

	/* Write tcam index - indirect access */
	mv_pp2x_write(pp2, MVPP2_PRS_TCAM_IDX_REG, pe->index);

	pe->tcam.word[MVPP2_PRS_TCAM_INV_WORD] = mv_pp2x_read(pp2,
			      MVPP2_PRS_TCAM_DATA_REG(MVPP2_PRS_TCAM_INV_WORD));
	if (pe->tcam.word[MVPP2_PRS_TCAM_INV_WORD] & MVPP2_PRS_TCAM_INV_MASK)
		return MVPP2_PRS_TCAM_ENTRY_INVALID;

	for (i = 0; i < MVPP2_PRS_TCAM_WORDS; i++)
		pe->tcam.word[i] = mv_pp2x_read(pp2, MVPP2_PRS_TCAM_DATA_REG(i));

	/* Write sram index - indirect access */
	mv_pp2x_write(pp2, MVPP2_PRS_SRAM_IDX_REG, pe->index);
	for (i = 0; i < MVPP2_PRS_SRAM_WORDS; i++)
		pe->sram.word[i] = mv_pp2x_read(pp2, MVPP2_PRS_SRAM_DATA_REG(i));

	return 0;
}

void mv_pp2x_prs_sw_clear(struct mv_pp2x_prs_entry *pe)
{
	memset(pe, 0, sizeof(struct mv_pp2x_prs_entry));
}

/* Invalidate tcam hw entry */
void mv_pp2x_prs_hw_inv(struct mv_pp2x *pp2, int index)
{
	/* Write index - indirect access */
	mv_pp2x_write(pp2, MVPP2_PRS_TCAM_IDX_REG, index);
	mv_pp2x_write(pp2, MVPP2_PRS_TCAM_DATA_REG(MVPP2_PRS_TCAM_INV_WORD),
		    MVPP2_PRS_TCAM_INV_MASK);
}

/* Enable shadow table entry and set its lookup ID */
static void mv_pp2x_prs_shadow_set(struct mv_pp2x *pp2, int index, int lu)
{
	pp2->prs_shadow[index].valid = true;
	pp2->prs_shadow[index].lu = lu;
}

/* Update ri fields in shadow table entry */
static void mv_pp2x_prs_shadow_ri_set(struct mv_pp2x *pp2, int index,
				    unsigned int ri, unsigned int ri_mask)
{
	pp2->prs_shadow[index].ri_mask = ri_mask;
	pp2->prs_shadow[index].ri = ri;
}

/* Update lookup field in tcam sw entry */
void mv_pp2x_prs_tcam_lu_set(struct mv_pp2x_prs_entry *pe, unsigned int lu)
{
	int enable_off = MVPP2_PRS_TCAM_EN_OFFS(MVPP2_PRS_TCAM_LU_BYTE);

	pe->tcam.byte[MVPP2_PRS_TCAM_LU_BYTE] = lu;
	pe->tcam.byte[enable_off] = MVPP2_PRS_LU_MASK;
}

/* Update mask for single port in tcam sw entry */
void mv_pp2x_prs_tcam_port_set(struct mv_pp2x_prs_entry *pe,
				    unsigned int port, bool add)
{
	int enable_off = MVPP2_PRS_TCAM_EN_OFFS(MVPP2_PRS_TCAM_PORT_BYTE);

	if (add)
		pe->tcam.byte[enable_off] &= ~(1 << port);
	else
		pe->tcam.byte[enable_off] |= 1 << port;
}

/* Update port map in tcam sw entry */
void mv_pp2x_prs_tcam_port_map_set(struct mv_pp2x_prs_entry *pe,
					unsigned int ports)
{
	unsigned char port_mask = MVPP2_PRS_PORT_MASK;
	int enable_off = MVPP2_PRS_TCAM_EN_OFFS(MVPP2_PRS_TCAM_PORT_BYTE);

	pe->tcam.byte[MVPP2_PRS_TCAM_PORT_BYTE] = 0;
	pe->tcam.byte[enable_off] &= ~port_mask;
	pe->tcam.byte[enable_off] |= ~ports & MVPP2_PRS_PORT_MASK;
}

/* Obtain port map from tcam sw entry */
static unsigned int mv_pp2x_prs_tcam_port_map_get(struct mv_pp2x_prs_entry *pe)
{
	int enable_off = MVPP2_PRS_TCAM_EN_OFFS(MVPP2_PRS_TCAM_PORT_BYTE);

	return ~(pe->tcam.byte[enable_off]) & MVPP2_PRS_PORT_MASK;
}

/* Set byte of data and its enable bits in tcam sw entry */
void mv_pp2x_prs_tcam_data_byte_set(struct mv_pp2x_prs_entry *pe,
					 unsigned int offs, unsigned char byte,
					 unsigned char enable)
{
	pe->tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE(offs)] = byte;
	pe->tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE_EN(offs)] = enable;
}

/* Get byte of data and its enable bits from tcam sw entry */
static void mv_pp2x_prs_tcam_data_byte_get(struct mv_pp2x_prs_entry *pe,
					 unsigned int offs, unsigned char *byte,
					 unsigned char *enable)
{
	*byte = pe->tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE(offs)];
	*enable = pe->tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE_EN(offs)];
}

/* Get dword of data and its enable bits from tcam sw entry */
static void mv_pp2x_prs_tcam_data_dword_get(struct mv_pp2x_prs_entry *pe,
					 unsigned int offs, unsigned int *word,
					 unsigned int *enable)
{
	int index, offset;
	unsigned char byte, mask;

	for (index = 0; index < 4; index++) {
		offset = (offs * 4) + index;
		mv_pp2x_prs_tcam_data_byte_get(pe, offset,  &byte, &mask);
		((unsigned char *)word)[index] = byte;
		((unsigned char *)enable)[index] = mask;
	}
}

/* Compare tcam data bytes with a pattern */
static bool mv_pp2x_prs_tcam_data_cmp(struct mv_pp2x_prs_entry *pe, int offs,
				    u16 data)
{
	int off = MVPP2_PRS_TCAM_DATA_BYTE(offs);
	u16 tcam_data;

	tcam_data = (8 << pe->tcam.byte[off + 1]) | pe->tcam.byte[off];
	if (tcam_data != data)
		return false;
	return true;
}

/* Update ai bits in tcam sw entry */
void mv_pp2x_prs_tcam_ai_update(struct mv_pp2x_prs_entry *pe,
				     unsigned int bits, unsigned int enable)
{
	int i, ai_idx = MVPP2_PRS_TCAM_AI_BYTE;

	for (i = 0; i < MVPP2_PRS_AI_BITS; i++) {
		if (!(enable & BIT(i)))
			continue;

		if (bits & BIT(i))
			pe->tcam.byte[ai_idx] |= 1 << i;
		else
			pe->tcam.byte[ai_idx] &= ~(1 << i);
	}

	pe->tcam.byte[MVPP2_PRS_TCAM_EN_OFFS(ai_idx)] |= enable;
}

/* Get ai bits from tcam sw entry */
static int mv_pp2x_prs_tcam_ai_get(struct mv_pp2x_prs_entry *pe)
{
	return pe->tcam.byte[MVPP2_PRS_TCAM_AI_BYTE];
}

/* Set ethertype in tcam sw entry */
static void mv_pp2x_prs_match_etype(struct mv_pp2x_prs_entry *pe, int offset,
				  unsigned short ethertype)
{
	mv_pp2x_prs_tcam_data_byte_set(pe, offset + 0, ethertype >> 8, 0xff);
	mv_pp2x_prs_tcam_data_byte_set(pe, offset + 1, ethertype & 0xff, 0xff);
}

/* Set bits in sram sw entry */
static void mv_pp2x_prs_sram_bits_set(struct mv_pp2x_prs_entry *pe, int bit_num,
				    int val)
{
	pe->sram.byte[MVPP2_BIT_TO_BYTE(bit_num)] |= (val << (bit_num % 8));
}

/* Clear bits in sram sw entry */
static void mv_pp2x_prs_sram_bits_clear(struct mv_pp2x_prs_entry *pe, int bit_num,
				      int val)
{
	pe->sram.byte[MVPP2_BIT_TO_BYTE(bit_num)] &= ~(val << (bit_num % 8));
}

/* Update ri bits in sram sw entry */
void mv_pp2x_prs_sram_ri_update(struct mv_pp2x_prs_entry *pe,
				     unsigned int bits, unsigned int mask)
{
	unsigned int i;

	for (i = 0; i < MVPP2_PRS_SRAM_RI_CTRL_BITS; i++) {
		int ri_off = MVPP2_PRS_SRAM_RI_OFFS;

		if (!(mask & BIT(i)))
			continue;

		if (bits & BIT(i))
			mv_pp2x_prs_sram_bits_set(pe, ri_off + i, 1);
		else
			mv_pp2x_prs_sram_bits_clear(pe, ri_off + i, 1);

		mv_pp2x_prs_sram_bits_set(pe,
			MVPP2_PRS_SRAM_RI_CTRL_OFFS + i, 1);
	}
}

/* Obtain ri bits from sram sw entry */
static int mv_pp2x_prs_sram_ri_get(struct mv_pp2x_prs_entry *pe)
{
	return pe->sram.word[MVPP2_PRS_SRAM_RI_WORD];
}

/* Update ai bits in sram sw entry */
void mv_pp2x_prs_sram_ai_update(struct mv_pp2x_prs_entry *pe,
				     unsigned int bits, unsigned int mask)
{
	unsigned int i;
	int ai_off = MVPP2_PRS_SRAM_AI_OFFS;

	for (i = 0; i < MVPP2_PRS_SRAM_AI_CTRL_BITS; i++) {
		if (!(mask & BIT(i)))
			continue;

		if (bits & BIT(i))
			mv_pp2x_prs_sram_bits_set(pe, ai_off + i, 1);
		else
			mv_pp2x_prs_sram_bits_clear(pe, ai_off + i, 1);

		mv_pp2x_prs_sram_bits_set(pe,
			MVPP2_PRS_SRAM_AI_CTRL_OFFS + i, 1);
	}
}

/* Read ai bits from sram sw entry */
static int mv_pp2x_prs_sram_ai_get(struct mv_pp2x_prs_entry *pe)
{
	u8 bits;
	int ai_off = MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_AI_OFFS);
	int ai_en_off = ai_off + 1;
	int ai_shift = MVPP2_PRS_SRAM_AI_OFFS % 8;

	bits = (pe->sram.byte[ai_off] >> ai_shift) |
	       (pe->sram.byte[ai_en_off] << (8 - ai_shift));

	return bits;
}

/* In sram sw entry set lookup ID field of the tcam key to be used in the next
 * lookup interation
 */
void mv_pp2x_prs_sram_next_lu_set(struct mv_pp2x_prs_entry *pe,
				       unsigned int lu)
{
	int sram_next_off = MVPP2_PRS_SRAM_NEXT_LU_OFFS;

	mv_pp2x_prs_sram_bits_clear(pe, sram_next_off,
				  MVPP2_PRS_SRAM_NEXT_LU_MASK);
	mv_pp2x_prs_sram_bits_set(pe, sram_next_off, lu);
}

/* In the sram sw entry set sign and value of the next lookup offset
 * and the offset value generated to the classifier
 */
static void mv_pp2x_prs_sram_shift_set(struct mv_pp2x_prs_entry *pe, int shift,
				     unsigned int op)
{
	/* Set sign */
	if (shift < 0) {
		mv_pp2x_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_SHIFT_SIGN_BIT, 1);
		shift = 0 - shift;
	} else {
		mv_pp2x_prs_sram_bits_clear(pe,
			MVPP2_PRS_SRAM_SHIFT_SIGN_BIT, 1);
	}

	/* Set value */
	pe->sram.byte[MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_SHIFT_OFFS)] =
							   (unsigned char)shift;

	/* Reset and set operation */
	mv_pp2x_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_OP_SEL_SHIFT_OFFS,
				  MVPP2_PRS_SRAM_OP_SEL_SHIFT_MASK);
	mv_pp2x_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_OP_SEL_SHIFT_OFFS, op);

	/* Set base offset as current */
	mv_pp2x_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_OP_SEL_BASE_OFFS, 1);
}

/* In the sram sw entry set sign and value of the user defined offset
 * generated to the classifier
 */
static void mv_pp2x_prs_sram_offset_set(struct mv_pp2x_prs_entry *pe,
				      unsigned int type, int offset,
				      unsigned int op)
{
	/* Set sign */
	if (offset < 0) {
		mv_pp2x_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_UDF_SIGN_BIT, 1);
		offset = 0 - offset;
	} else {
		mv_pp2x_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_UDF_SIGN_BIT, 1);
	}

	/* Set value */
	mv_pp2x_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_UDF_OFFS,
				  MVPP2_PRS_SRAM_UDF_MASK);
	mv_pp2x_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_UDF_OFFS, offset);
	pe->sram.byte[MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_UDF_OFFS +
					MVPP2_PRS_SRAM_UDF_BITS)] &=
	      ~(MVPP2_PRS_SRAM_UDF_MASK >> (8 - (MVPP2_PRS_SRAM_UDF_OFFS % 8)));
	pe->sram.byte[MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_UDF_OFFS +
					MVPP2_PRS_SRAM_UDF_BITS)] |=
				(offset >> (8 - (MVPP2_PRS_SRAM_UDF_OFFS % 8)));

	/* Set offset type */
	mv_pp2x_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_UDF_TYPE_OFFS,
				  MVPP2_PRS_SRAM_UDF_TYPE_MASK);
	mv_pp2x_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_UDF_TYPE_OFFS, type);

	/* Set offset operation */
	mv_pp2x_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_MASK);
	mv_pp2x_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS, op);

	pe->sram.byte[MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS +
					MVPP2_PRS_SRAM_OP_SEL_UDF_BITS)] &=
					     ~(MVPP2_PRS_SRAM_OP_SEL_UDF_MASK >>
				    (8 - (MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS % 8)));

	pe->sram.byte[MVPP2_BIT_TO_BYTE(MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS +
					MVPP2_PRS_SRAM_OP_SEL_UDF_BITS)] |=
			     (op >> (8 - (MVPP2_PRS_SRAM_OP_SEL_UDF_OFFS % 8)));

	/* Set base offset as current */
	mv_pp2x_prs_sram_bits_clear(pe, MVPP2_PRS_SRAM_OP_SEL_BASE_OFFS, 1);
}

/* Return first free tcam index, seeking from start to end */
static int mv_pp2x_prs_tcam_first_free(struct mv_pp2x *pp2, unsigned char start,
				     unsigned char end)
{
	int tid;

	if (start > end)
		swap(start, end);

	if (end >= MVPP2_PRS_TCAM_SRAM_SIZE)
		end = MVPP2_PRS_TCAM_SRAM_SIZE - 1;

	for (tid = start; tid <= end; tid++) {
		if (!pp2->prs_shadow[tid].valid)
			return tid;
	}

	return -EINVAL;
}

/* Enable/disable dropping all mac da's */
static void mv_pp2x_prs_mac_drop_all_set(struct mv_pp2x *pp2, int port, bool add)
{
	struct mv_pp2x_prs_entry pe;

	if (pp2->prs_shadow[MVPP2_PE_DROP_ALL].valid) {
		/* Entry exist - update port only */
		pe.index = MVPP2_PE_DROP_ALL;
		mv_pp2x_prs_hw_read(pp2, &pe);
	} else {
		/* Entry doesn't exist - create new */
		memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
		mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_MAC);
		pe.index = MVPP2_PE_DROP_ALL;

		/* Non-promiscuous mode for all ports - DROP unknown packets */
		mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_DROP_MASK,
					 MVPP2_PRS_RI_DROP_MASK);

		mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
		mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);

		/* Update shadow table */
		mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_MAC);

		/* Mask all ports */
		mv_pp2x_prs_tcam_port_map_set(&pe, 0);
	}

	/* Update port mask */
	mv_pp2x_prs_tcam_port_set(&pe, port, add);

	mv_pp2x_prs_hw_write(pp2, &pe);
}

/* Set port to promiscuous mode */
void mv_pp2x_prs_mac_promisc_set(struct mv_pp2x *pp2, int port, bool add)
{
	struct mv_pp2x_prs_entry pe;

	/* Promiscous mode - Accept unknown packets */

	if (pp2->prs_shadow[MVPP2_PE_MAC_PROMISCUOUS].valid) {
		/* Entry exist - update port only */
		pe.index = MVPP2_PE_MAC_PROMISCUOUS;
		mv_pp2x_prs_hw_read(pp2, &pe);
	} else {
		/* Entry doesn't exist - create new */
		memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
		mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_MAC);
		pe.index = MVPP2_PE_MAC_PROMISCUOUS;

		/* Continue - set next lookup */
		mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_DSA);

		/* Set result info bits */
		mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L2_UCAST,
					 MVPP2_PRS_RI_L2_CAST_MASK);

		/* Shift to ethertype */
		mv_pp2x_prs_sram_shift_set(&pe, 2 * ETH_ALEN,
					 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

		/* Mask all ports */
		mv_pp2x_prs_tcam_port_map_set(&pe, 0);

		/* Update shadow table */
		mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_MAC);
	}

	/* Update port mask */
	mv_pp2x_prs_tcam_port_set(&pe, port, add);

	mv_pp2x_prs_hw_write(pp2, &pe);
}

/* Accept multicast */
void mv_pp2x_prs_mac_multi_set(struct mv_pp2x *pp2, int port, int index,
				    bool add)
{
	struct mv_pp2x_prs_entry pe;
	unsigned char da_mc;

	/* Ethernet multicast address first byte is
	 * 0x01 for IPv4 and 0x33 for IPv6
	 */
	da_mc = (index == MVPP2_PE_MAC_MC_ALL) ? 0x01 : 0x33;

	if (pp2->prs_shadow[index].valid) {
		/* Entry exist - update port only */
		pe.index = index;
		mv_pp2x_prs_hw_read(pp2, &pe);
	} else {
		/* Entry doesn't exist - create new */
		memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
		mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_MAC);
		pe.index = index;

		/* Continue - set next lookup */
		mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_DSA);

		/* Set result info bits */
		mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L2_MCAST,
					 MVPP2_PRS_RI_L2_CAST_MASK);

		/* Update tcam entry data first byte */
		mv_pp2x_prs_tcam_data_byte_set(&pe, 0, da_mc, 0xff);

		/* Shift to ethertype */
		mv_pp2x_prs_sram_shift_set(&pe, 2 * ETH_ALEN,
					 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

		/* Mask all ports */
		mv_pp2x_prs_tcam_port_map_set(&pe, 0);

		/* Update shadow table */
		mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_MAC);
	}

	/* Update port mask */
	mv_pp2x_prs_tcam_port_set(&pe, port, add);

	mv_pp2x_prs_hw_write(pp2, &pe);
}

/* Set entry for dsa packets */
static void mv_pp2x_prs_dsa_tag_set(struct mv_pp2x *pp2, int port, bool add,
				  bool tagged, bool extend)
{
	struct mv_pp2x_prs_entry pe;
	int tid, shift;

	if (extend) {
		tid = tagged ? MVPP2_PE_EDSA_TAGGED : MVPP2_PE_EDSA_UNTAGGED;
		shift = 8;
	} else {
		tid = tagged ? MVPP2_PE_DSA_TAGGED : MVPP2_PE_DSA_UNTAGGED;
		shift = 4;
	}

	if (pp2->prs_shadow[tid].valid) {
		/* Entry exist - update port only */
		pe.index = tid;
		mv_pp2x_prs_hw_read(pp2, &pe);
	} else {
		/* Entry doesn't exist - create new */
		memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
		mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_DSA);
		pe.index = tid;

		/* Shift 4 bytes if DSA tag or 8 bytes in case of EDSA tag*/
		mv_pp2x_prs_sram_shift_set(&pe, shift,
					 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

		/* Update shadow table */
		mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_DSA);

		if (tagged) {
			/* Set tagged bit in DSA tag */
			mv_pp2x_prs_tcam_data_byte_set(&pe, 0,
						 MVPP2_PRS_TCAM_DSA_TAGGED_BIT,
						 MVPP2_PRS_TCAM_DSA_TAGGED_BIT);
			/* Clear all ai bits for next iteration */
			mv_pp2x_prs_sram_ai_update(&pe, 0,
						 MVPP2_PRS_SRAM_AI_MASK);
			/* If packet is tagged continue check vlans */
			mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_VLAN);
		} else {
			/* Set result info bits to 'no vlans' */
			mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_NONE,
						 MVPP2_PRS_RI_VLAN_MASK);
			mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_L2);
		}

		/* Mask all ports */
		mv_pp2x_prs_tcam_port_map_set(&pe, 0);
	}

	/* Update port mask */
	mv_pp2x_prs_tcam_port_set(&pe, port, add);

	mv_pp2x_prs_hw_write(pp2, &pe);
}

/* Set entry for dsa ethertype */
static void mv_pp2x_prs_dsa_tag_ethertype_set(struct mv_pp2x *pp2, int port,
					    bool add, bool tagged, bool extend)
{
	struct mv_pp2x_prs_entry pe;
	int tid, shift, port_mask;

	if (extend) {
		tid = tagged ? MVPP2_PE_ETYPE_EDSA_TAGGED :
		      MVPP2_PE_ETYPE_EDSA_UNTAGGED;
		port_mask = 0;
		shift = 8;
	} else {
		tid = tagged ? MVPP2_PE_ETYPE_DSA_TAGGED :
		      MVPP2_PE_ETYPE_DSA_UNTAGGED;
		port_mask = MVPP2_PRS_PORT_MASK;
		shift = 4;
	}

	if (pp2->prs_shadow[tid].valid) {
		/* Entry exist - update port only */
		pe.index = tid;
		mv_pp2x_prs_hw_read(pp2, &pe);
	} else {
		/* Entry doesn't exist - create new */
		memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
		mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_DSA);
		pe.index = tid;

		/* Set ethertype */
		mv_pp2x_prs_match_etype(&pe, 0, MV_EDSA_TYPE);
		mv_pp2x_prs_match_etype(&pe, 2, 0);

		mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_DSA_MASK,
					 MVPP2_PRS_RI_DSA_MASK);
		/* Shift ethertype + 2 byte reserved + tag*/
		mv_pp2x_prs_sram_shift_set(&pe, 2 + MVPP2_ETH_TYPE_LEN + shift,
					 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

		/* Update shadow table */
		mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_DSA);

		if (tagged) {
			/* Set tagged bit in DSA tag */
			mv_pp2x_prs_tcam_data_byte_set(&pe,
						     MVPP2_ETH_TYPE_LEN + 2 + 3,
						 MVPP2_PRS_TCAM_DSA_TAGGED_BIT,
						 MVPP2_PRS_TCAM_DSA_TAGGED_BIT);
			/* Clear all ai bits for next iteration */
			mv_pp2x_prs_sram_ai_update(&pe, 0,
						 MVPP2_PRS_SRAM_AI_MASK);
			/* If packet is tagged continue check vlans */
			mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_VLAN);
		} else {
			/* Set result info bits to 'no vlans' */
			mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_NONE,
						 MVPP2_PRS_RI_VLAN_MASK);
			mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_L2);
		}
		/* Mask/unmask all ports, depending on dsa type */
		mv_pp2x_prs_tcam_port_map_set(&pe, port_mask);
	}

	/* Update port mask */
	mv_pp2x_prs_tcam_port_set(&pe, port, add);

	mv_pp2x_prs_hw_write(pp2, &pe);
}

/* Search for existing single/triple vlan entry */
static struct mv_pp2x_prs_entry *mv_pp2x_prs_vlan_find(struct mv_pp2x *pp2,
						   unsigned short tpid, int ai)
{
	struct mv_pp2x_prs_entry *pe;
	int tid;

	pe = kzalloc(sizeof(*pe), GFP_KERNEL);
	if (!pe)
		return NULL;
	mv_pp2x_prs_tcam_lu_set(pe, MVPP2_PRS_LU_VLAN);

	/* Go through the all entries with MVPP2_PRS_LU_VLAN */
	for (tid = MVPP2_PE_FIRST_FREE_TID;
	     tid <= MVPP2_PE_LAST_FREE_TID; tid++) {
		unsigned int ri_bits, ai_bits;
		bool match;

		if (!pp2->prs_shadow[tid].valid ||
		    pp2->prs_shadow[tid].lu != MVPP2_PRS_LU_VLAN)
			continue;

		pe->index = tid;

		mv_pp2x_prs_hw_read(pp2, pe);
		match = mv_pp2x_prs_tcam_data_cmp(pe, 0, swab16(tpid));
		if (!match)
			continue;

		/* Get vlan type */
		ri_bits = mv_pp2x_prs_sram_ri_get(pe);
		ri_bits &= MVPP2_PRS_RI_VLAN_MASK;

		/* Get current ai value from tcam */
		ai_bits = mv_pp2x_prs_tcam_ai_get(pe);
		/* Clear double vlan bit */
		ai_bits &= ~MVPP2_PRS_DBL_VLAN_AI_BIT;

		if (ai != ai_bits)
			continue;

		if (ri_bits == MVPP2_PRS_RI_VLAN_SINGLE ||
		    ri_bits == MVPP2_PRS_RI_VLAN_TRIPLE)
			return pe;
	}
	kfree(pe);

	return NULL;
}

/* Add/update single/triple vlan entry */
static int mv_pp2x_prs_vlan_add(struct mv_pp2x *pp2, unsigned short tpid, int ai,
			      unsigned int port_map)
{
	struct mv_pp2x_prs_entry *pe;
	int tid_aux, tid;
	int ret = 0;

	pe = mv_pp2x_prs_vlan_find(pp2, tpid, ai);

	if (!pe) {
		/* Create new tcam entry */
		tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_LAST_FREE_TID,
						MVPP2_PE_FIRST_FREE_TID);
		if (tid < 0)
			return tid;

		pe = kzalloc(sizeof(*pe), GFP_KERNEL);
		if (!pe)
			return -ENOMEM;

		/* Get last double vlan tid */
		for (tid_aux = MVPP2_PE_LAST_FREE_TID;
		     tid_aux >= MVPP2_PE_FIRST_FREE_TID; tid_aux--) {
			unsigned int ri_bits;

			if (!pp2->prs_shadow[tid_aux].valid ||
			    pp2->prs_shadow[tid_aux].lu != MVPP2_PRS_LU_VLAN)
				continue;

			pe->index = tid_aux;
			mv_pp2x_prs_hw_read(pp2, pe);
			ri_bits = mv_pp2x_prs_sram_ri_get(pe);
			if ((ri_bits & MVPP2_PRS_RI_VLAN_MASK) ==
			    MVPP2_PRS_RI_VLAN_DOUBLE)
				break;
		}

		if (tid <= tid_aux) {
			ret = -EINVAL;
			goto error;
		}

		memset(pe, 0 , sizeof(struct mv_pp2x_prs_entry));
		mv_pp2x_prs_tcam_lu_set(pe, MVPP2_PRS_LU_VLAN);
		pe->index = tid;

		mv_pp2x_prs_match_etype(pe, 0, tpid);

		mv_pp2x_prs_sram_next_lu_set(pe, MVPP2_PRS_LU_L2);
		/* Shift 4 bytes - skip 1 vlan tag */
		mv_pp2x_prs_sram_shift_set(pe, MVPP2_VLAN_TAG_LEN,
					 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
		/* Clear all ai bits for next iteration */
		mv_pp2x_prs_sram_ai_update(pe, 0, MVPP2_PRS_SRAM_AI_MASK);

		if (ai == MVPP2_PRS_SINGLE_VLAN_AI) {
			mv_pp2x_prs_sram_ri_update(pe, MVPP2_PRS_RI_VLAN_SINGLE,
						 MVPP2_PRS_RI_VLAN_MASK);
		} else {
			ai |= MVPP2_PRS_DBL_VLAN_AI_BIT;
			mv_pp2x_prs_sram_ri_update(pe, MVPP2_PRS_RI_VLAN_TRIPLE,
						 MVPP2_PRS_RI_VLAN_MASK);
		}
		mv_pp2x_prs_tcam_ai_update(pe, ai, MVPP2_PRS_SRAM_AI_MASK);

		mv_pp2x_prs_shadow_set(pp2, pe->index, MVPP2_PRS_LU_VLAN);
	}
	/* Update ports' mask */
	mv_pp2x_prs_tcam_port_map_set(pe, port_map);

	mv_pp2x_prs_hw_write(pp2, pe);

error:
	kfree(pe);

	return ret;
}

/* Get first free double vlan ai number */
static int mv_pp2x_prs_double_vlan_ai_free_get(struct mv_pp2x *pp2)
{
	int i;

	for (i = 1; i < MVPP2_PRS_DBL_VLANS_MAX; i++) {
		if (!pp2->prs_double_vlans[i])
			return i;
	}

	return -EINVAL;
}

/* Search for existing double vlan entry */
static struct mv_pp2x_prs_entry *mv_pp2x_prs_double_vlan_find(struct mv_pp2x *pp2,
							  unsigned short tpid1,
							  unsigned short tpid2)
{
	struct mv_pp2x_prs_entry *pe;
	int tid;

	pe = kzalloc(sizeof(*pe), GFP_KERNEL);
	if (!pe)
		return NULL;
	mv_pp2x_prs_tcam_lu_set(pe, MVPP2_PRS_LU_VLAN);

	/* Go through the all entries with MVPP2_PRS_LU_VLAN */
	for (tid = MVPP2_PE_FIRST_FREE_TID;
	     tid <= MVPP2_PE_LAST_FREE_TID; tid++) {
		unsigned int ri_mask;
		bool match;

		if (!pp2->prs_shadow[tid].valid ||
		    pp2->prs_shadow[tid].lu != MVPP2_PRS_LU_VLAN)
			continue;

		pe->index = tid;
		mv_pp2x_prs_hw_read(pp2, pe);

		match = mv_pp2x_prs_tcam_data_cmp(pe, 0, swab16(tpid1))
			&& mv_pp2x_prs_tcam_data_cmp(pe, 4, swab16(tpid2));

		if (!match)
			continue;

		ri_mask = mv_pp2x_prs_sram_ri_get(pe) & MVPP2_PRS_RI_VLAN_MASK;
		if (ri_mask == MVPP2_PRS_RI_VLAN_DOUBLE)
			return pe;
	}
	kfree(pe);

	return NULL;
}

/* Add or update double vlan entry */
static int mv_pp2x_prs_double_vlan_add(struct mv_pp2x *pp2, unsigned short tpid1,
				     unsigned short tpid2,
				     unsigned int port_map)
{
	struct mv_pp2x_prs_entry *pe;
	int tid_aux, tid, ai, ret = 0;

	pe = mv_pp2x_prs_double_vlan_find(pp2, tpid1, tpid2);

	if (!pe) {
		/* Create new tcam entry */
		tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
				MVPP2_PE_LAST_FREE_TID);
		if (tid < 0)
			return tid;

		pe = kzalloc(sizeof(*pe), GFP_KERNEL);
		if (!pe)
			return -ENOMEM;

		/* Set ai value for new double vlan entry */
		ai = mv_pp2x_prs_double_vlan_ai_free_get(pp2);
		if (ai < 0) {
			ret = ai;
			goto error;
		}

		/* Get first single/triple vlan tid */
		for (tid_aux = MVPP2_PE_FIRST_FREE_TID;
		     tid_aux <= MVPP2_PE_LAST_FREE_TID; tid_aux++) {
			unsigned int ri_bits;

			if (!pp2->prs_shadow[tid_aux].valid ||
			    pp2->prs_shadow[tid_aux].lu != MVPP2_PRS_LU_VLAN)
				continue;

			pe->index = tid_aux;
			mv_pp2x_prs_hw_read(pp2, pe);
			ri_bits = mv_pp2x_prs_sram_ri_get(pe);
			ri_bits &= MVPP2_PRS_RI_VLAN_MASK;
			if (ri_bits == MVPP2_PRS_RI_VLAN_SINGLE ||
			    ri_bits == MVPP2_PRS_RI_VLAN_TRIPLE)
				break;
		}

		if (tid >= tid_aux) {
			ret = -ERANGE;
			goto error;
		}

		memset(pe, 0, sizeof(struct mv_pp2x_prs_entry));
		mv_pp2x_prs_tcam_lu_set(pe, MVPP2_PRS_LU_VLAN);
		pe->index = tid;

		pp2->prs_double_vlans[ai] = true;

		mv_pp2x_prs_match_etype(pe, 0, tpid1);
		mv_pp2x_prs_match_etype(pe, 4, tpid2);

		mv_pp2x_prs_sram_next_lu_set(pe, MVPP2_PRS_LU_VLAN);
		/* Shift 8 bytes - skip 2 vlan tags */
		mv_pp2x_prs_sram_shift_set(pe, 2 * MVPP2_VLAN_TAG_LEN,
					 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
		mv_pp2x_prs_sram_ri_update(pe, MVPP2_PRS_RI_VLAN_DOUBLE,
					 MVPP2_PRS_RI_VLAN_MASK);
		mv_pp2x_prs_sram_ai_update(pe, ai | MVPP2_PRS_DBL_VLAN_AI_BIT,
					 MVPP2_PRS_SRAM_AI_MASK);

		mv_pp2x_prs_shadow_set(pp2, pe->index, MVPP2_PRS_LU_VLAN);
	}

	/* Update ports' mask */
	mv_pp2x_prs_tcam_port_map_set(pe, port_map);
	mv_pp2x_prs_hw_write(pp2, pe);

error:
	kfree(pe);
	return ret;
}

/* IPv4 header parsing for fragmentation and L4 offset */
static int mv_pp2x_prs_ip4_proto(struct mv_pp2x *pp2, unsigned short proto,
			       unsigned int ri, unsigned int ri_mask)
{
	struct mv_pp2x_prs_entry pe;
	int tid;

	if ((proto != 6/*IPPROTO_TCP*/) && (proto != IPPROTO_UDP) &&
	    (proto != 2/*IPPROTO_IGMP*/))
		return -EINVAL;

	/* Not fragmented packet */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP4);
	pe.index = tid;

	/* Set next lu to IPv4 */
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_sram_shift_set(&pe, 12, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	/* Set L4 offset */
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L4,
				  sizeof(struct ip_hdr) - 4,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);
	mv_pp2x_prs_sram_ai_update(&pe, MVPP2_PRS_IPV4_DIP_AI_BIT,
				 MVPP2_PRS_IPV4_DIP_AI_BIT);
	mv_pp2x_prs_sram_ri_update(&pe, ri | MVPP2_PRS_RI_IP_FRAG_FALSE,
				 ri_mask | MVPP2_PRS_RI_IP_FRAG_MASK);

	mv_pp2x_prs_tcam_data_byte_set(&pe, 2, 0x00,
		MVPP2_PRS_TCAM_PROTO_MASK_L);
	mv_pp2x_prs_tcam_data_byte_set(&pe, 3, 0x00, MVPP2_PRS_TCAM_PROTO_MASK);
	mv_pp2x_prs_tcam_data_byte_set(&pe, 5, proto,
		MVPP2_PRS_TCAM_PROTO_MASK);
	mv_pp2x_prs_tcam_ai_update(&pe, 0, MVPP2_PRS_IPV4_DIP_AI_BIT);
	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Fragmented packet */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	pe.index = tid;
	/* Clear ri before updating */
	pe.sram.word[MVPP2_PRS_SRAM_RI_WORD] = 0x0;
	pe.sram.word[MVPP2_PRS_SRAM_RI_CTRL_WORD] = 0x0;
	mv_pp2x_prs_sram_ri_update(&pe, ri, ri_mask);
	mv_pp2x_prs_sram_ri_update(&pe, ri | MVPP2_PRS_RI_IP_FRAG_TRUE,
				 ri_mask | MVPP2_PRS_RI_IP_FRAG_MASK);

	mv_pp2x_prs_tcam_data_byte_set(&pe, 2, 0x00, 0x0);
	mv_pp2x_prs_tcam_data_byte_set(&pe, 3, 0x00, 0x0);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_hw_write(pp2, &pe);

	return 0;
}

/* IPv4 L3 multicast or broadcast */
static int mv_pp2x_prs_ip4_cast(struct mv_pp2x *pp2, unsigned short l3_cast)
{
	struct mv_pp2x_prs_entry pe;
	int mask, tid;

	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP4);
	pe.index = tid;

	switch (l3_cast) {
	case MVPP2_PRS_L3_MULTI_CAST:
		mv_pp2x_prs_tcam_data_byte_set(&pe, 0, MVPP2_PRS_IPV4_MC,
					     MVPP2_PRS_IPV4_MC_MASK);
		mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_MCAST,
					 MVPP2_PRS_RI_L3_ADDR_MASK);
		break;
	case  MVPP2_PRS_L3_BROAD_CAST:
		mask = MVPP2_PRS_IPV4_BC_MASK;
		mv_pp2x_prs_tcam_data_byte_set(&pe, 0, mask, mask);
		mv_pp2x_prs_tcam_data_byte_set(&pe, 1, mask, mask);
		mv_pp2x_prs_tcam_data_byte_set(&pe, 2, mask, mask);
		mv_pp2x_prs_tcam_data_byte_set(&pe, 3, mask, mask);
		mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_BCAST,
					 MVPP2_PRS_RI_L3_ADDR_MASK);
		break;
	default:
		return -EINVAL;
	}

	/* Finished: go to flowid generation */
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);

	mv_pp2x_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV4_DIP_AI_BIT,
				 MVPP2_PRS_IPV4_DIP_AI_BIT);
	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_hw_write(pp2, &pe);

	return 0;
}

/* Set entries for protocols over IPv6  */
static int mv_pp2x_prs_ip6_proto(struct mv_pp2x *pp2, unsigned short proto,
			       unsigned int ri, unsigned int ri_mask)
{
	struct mv_pp2x_prs_entry pe;
	int tid;

	if ((proto != 6/*IPPROTO_TCP*/) && (proto != IPPROTO_UDP) &&
	    (proto != 58/*IPPROTO_ICMPV6*/) && (proto != 4/*IPPROTO_IPIP*/))
		return -EINVAL;

	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = tid;

	/* Finished: go to flowid generation */
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mv_pp2x_prs_sram_ri_update(&pe, ri, ri_mask);
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L4,
				  /*sizeof(struct ip6_hdr)*/40 - 6,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	mv_pp2x_prs_tcam_data_byte_set(&pe, 0, proto,
				MVPP2_PRS_TCAM_PROTO_MASK);
	mv_pp2x_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV6_NO_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Write HW */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_IP6);
	mv_pp2x_prs_hw_write(pp2, &pe);

	return 0;
}

/* IPv6 L3 multicast entry */
static int mv_pp2x_prs_ip6_cast(struct mv_pp2x *pp2, unsigned short l3_cast)
{
	struct mv_pp2x_prs_entry pe;
	int tid;

	if (l3_cast != MVPP2_PRS_L3_MULTI_CAST)
		return -EINVAL;

	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = tid;

	/* Finished: go to flowid generation */
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP6);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_MCAST,
				 MVPP2_PRS_RI_L3_ADDR_MASK);
	mv_pp2x_prs_sram_ai_update(&pe, MVPP2_PRS_IPV6_NO_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Shift back to IPv6 NH */
	mv_pp2x_prs_sram_shift_set(&pe, -18, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

	mv_pp2x_prs_tcam_data_byte_set(&pe, 0, MVPP2_PRS_IPV6_MC,
				     MVPP2_PRS_IPV6_MC_MASK);
	mv_pp2x_prs_tcam_ai_update(&pe, 0, MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_IP6);
	mv_pp2x_prs_hw_write(pp2, &pe);

	return 0;
}

/* Parser per-port initialization */
void mv_pp2x_prs_hw_port_init(struct mv_pp2x *pp2, int port, int lu_first,
				   int lu_max, int offset)
{
	u32 val;

	/* Set lookup ID */
	val = mv_pp2x_read(pp2, MVPP2_PRS_INIT_LOOKUP_REG);
	val &= ~MVPP2_PRS_PORT_LU_MASK(port);
	val |=  MVPP2_PRS_PORT_LU_VAL(port, lu_first);
	mv_pp2x_write(pp2, MVPP2_PRS_INIT_LOOKUP_REG, val);

	/* Set maximum number of loops for packet received from port */
	val = mv_pp2x_read(pp2, MVPP2_PRS_MAX_LOOP_REG(port));
	val &= ~MVPP2_PRS_MAX_LOOP_MASK(port);
	val |= MVPP2_PRS_MAX_LOOP_VAL(port, lu_max);
	mv_pp2x_write(pp2, MVPP2_PRS_MAX_LOOP_REG(port), val);

	/* Set initial offset for packet header extraction for the first
	 * searching loop
	 */
	val = mv_pp2x_read(pp2, MVPP2_PRS_INIT_OFFS_REG(port));
	val &= ~MVPP2_PRS_INIT_OFF_MASK(port);
	val |= MVPP2_PRS_INIT_OFF_VAL(port, offset);
	mv_pp2x_write(pp2, MVPP2_PRS_INIT_OFFS_REG(port), val);
}

/* Default flow entries initialization for all ports */
static void mv_pp2x_prs_def_flow_init(struct mv_pp2x *pp2)
{
	struct mv_pp2x_prs_entry pe;
	int port;

	for (port = 0; port < CONFIG_MAX_PP2_PORT_NUM; port++) {
		memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
		mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
		pe.index = MVPP2_PE_FIRST_DEFAULT_FLOW - port;

		/* Mask all ports */
		mv_pp2x_prs_tcam_port_map_set(&pe, 0);

		/* Set flow ID*/
		mv_pp2x_prs_sram_ai_update(&pe, port, MVPP2_PRS_FLOW_ID_MASK);
		mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_DONE_BIT, 1);

		/* Update shadow table and hw entry */
		mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_FLOWS);
		mv_pp2x_prs_hw_write(pp2, &pe);
	}
}

/* Set default entry for Marvell Header field */
static void mv_pp2x_prs_mh_init(struct mv_pp2x *pp2)
{
	struct mv_pp2x_prs_entry pe;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));

	pe.index = MVPP2_PE_MH_DEFAULT;
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_MH);
	mv_pp2x_prs_sram_shift_set(&pe, MVPP2_MH_SIZE,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_MAC);

	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_MH);
	mv_pp2x_prs_hw_write(pp2, &pe);
}

/* Set default entires (place holder) for promiscuous, non-promiscuous and
 * multicast MAC addresses
 */
static void mv_pp2x_prs_mac_init(struct mv_pp2x *pp2)
{
	struct mv_pp2x_prs_entry pe;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));

	/* Non-promiscuous mode for all ports - DROP unknown packets */
	pe.index = MVPP2_PE_MAC_NON_PROMISCUOUS;
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_MAC);

	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_DROP_MASK,
				 MVPP2_PRS_RI_DROP_MASK);
	mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);

	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_MAC);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* place holders only - no ports */
	mv_pp2x_prs_mac_drop_all_set(pp2, 0, false);
	mv_pp2x_prs_mac_promisc_set(pp2, 0, false);
	mv_pp2x_prs_mac_multi_set(pp2, MVPP2_PE_MAC_MC_ALL, 0, false);
	mv_pp2x_prs_mac_multi_set(pp2, MVPP2_PE_MAC_MC_IP6, 0, false);
}

/* Compare MAC DA with tcam entry data */
static bool mv_pp2x_prs_mac_range_equals(struct mv_pp2x_prs_entry *pe,
				       const u8 *da, unsigned char *mask)
{
	unsigned char tcam_byte, tcam_mask;
	int index;

	for (index = 0; index < ETH_ALEN; index++) {
		mv_pp2x_prs_tcam_data_byte_get(pe, index,
					&tcam_byte, &tcam_mask);
		if (tcam_mask != mask[index])
			return false;

		if ((tcam_mask & tcam_byte) != (da[index] & mask[index]))
			return false;
	}

	return true;
}

/* Find tcam entry with matched pair <MAC DA, port> */
static struct mv_pp2x_prs_entry *
mv_pp2x_prs_mac_da_range_find(struct mv_pp2x *pp2, int pmap, const u8 *da,
			    unsigned char *mask, int udf_type)
{
	struct mv_pp2x_prs_entry *pe;
	int tid;

	pe = kzalloc(sizeof(*pe), GFP_KERNEL);
	if (!pe)
		return NULL;
	mv_pp2x_prs_tcam_lu_set(pe, MVPP2_PRS_LU_MAC);

	/* Go through the all entires with MVPP2_PRS_LU_MAC */
	for (tid = MVPP2_PE_FIRST_FREE_TID;
	     tid <= MVPP2_PE_LAST_FREE_TID; tid++) {
		unsigned int entry_pmap;

		if (!pp2->prs_shadow[tid].valid ||
		    (pp2->prs_shadow[tid].lu != MVPP2_PRS_LU_MAC) ||
		    (pp2->prs_shadow[tid].udf != udf_type))
			continue;

		pe->index = tid;
		mv_pp2x_prs_hw_read(pp2, pe);
		entry_pmap = mv_pp2x_prs_tcam_port_map_get(pe);

		if (mv_pp2x_prs_mac_range_equals(pe, da, mask) &&
		    entry_pmap == pmap)
			return pe;
	}
	kfree(pe);

	return NULL;
}

/* Update parser's mac da entry */
int mv_pp2x_prs_mac_da_accept(struct mv_pp2x_port *pp, const u8 *da, bool add)
{
	struct mv_pp2x_prs_entry *pe;
	unsigned int pmap, len, ri;
	unsigned char mask[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	int tid;

	/* Scan TCAM and see if entry with this <MAC DA, port> already exist */
	pe = mv_pp2x_prs_mac_da_range_find(pp->pp2, (1 << pp->id), da, mask,
					 MVPP2_PRS_UDF_MAC_DEF);

	/* No such entry */
	if (!pe) {
		if (!add)
			return 0;

		/* Create new TCAM entry */
		/* Find first range mac entry*/
		for (tid = MVPP2_PE_FIRST_FREE_TID;
		     tid <= MVPP2_PE_LAST_FREE_TID; tid++)
			if (pp->pp2->prs_shadow[tid].valid &&
			    (pp->pp2->prs_shadow[tid].lu == MVPP2_PRS_LU_MAC) &&
			    (pp->pp2->prs_shadow[tid].udf ==
						       MVPP2_PRS_UDF_MAC_RANGE))
				break;

		/* Go through the all entries from first to last */
		tid = mv_pp2x_prs_tcam_first_free(pp->pp2,
			MVPP2_PE_FIRST_FREE_TID, tid - 1);
		if (tid < 0)
			return tid;

		pe = kzalloc(sizeof(*pe), GFP_KERNEL);
		if (!pe)
			return -1;
		mv_pp2x_prs_tcam_lu_set(pe, MVPP2_PRS_LU_MAC);
		pe->index = tid;

		/* Mask all ports */
		mv_pp2x_prs_tcam_port_map_set(pe, 0);
	}

	/* Update port mask */
	mv_pp2x_prs_tcam_port_set(pe, pp->id, add);

	/* Invalidate the entry if no ports are left enabled */
	pmap = mv_pp2x_prs_tcam_port_map_get(pe);
	if (pmap == 0) {
		if (add) {
			kfree(pe);
			return -1;
		}
		mv_pp2x_prs_hw_inv(pp->pp2, pe->index);
		pp->pp2->prs_shadow[pe->index].valid = false;
		kfree(pe);
		return 0;
	}

	/* Continue - set next lookup */
	mv_pp2x_prs_sram_next_lu_set(pe, MVPP2_PRS_LU_DSA);

	/* Set match on DA */
	len = ETH_ALEN;
	while (len--)
		mv_pp2x_prs_tcam_data_byte_set(pe, len, da[len], 0xff);

	/* Set result info bits */
	if (is_broadcast_ether_addr(da))
		ri = MVPP2_PRS_RI_L2_BCAST;
	else if (is_multicast_ether_addr(da))
		ri = MVPP2_PRS_RI_L2_MCAST;
	else
		ri = MVPP2_PRS_RI_L2_UCAST | MVPP2_PRS_RI_MAC_ME_MASK;

	mv_pp2x_prs_sram_ri_update(pe, ri, MVPP2_PRS_RI_L2_CAST_MASK |
				 MVPP2_PRS_RI_MAC_ME_MASK);
	mv_pp2x_prs_shadow_ri_set(pp->pp2, pe->index, ri,
			MVPP2_PRS_RI_L2_CAST_MASK | MVPP2_PRS_RI_MAC_ME_MASK);

	/* Shift to ethertype */
	mv_pp2x_prs_sram_shift_set(pe, 2 * ETH_ALEN,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

	/* Update shadow table and hw entry */
	pp->pp2->prs_shadow[pe->index].udf = MVPP2_PRS_UDF_MAC_DEF;
	mv_pp2x_prs_shadow_set(pp->pp2, pe->index, MVPP2_PRS_LU_MAC);
	mv_pp2x_prs_hw_write(pp->pp2, pe);

	kfree(pe);

	return 0;
}

/* Set default entries for various types of dsa packets */
static void mv_pp2x_prs_dsa_init(struct mv_pp2x *pp2)
{
	struct mv_pp2x_prs_entry pe;

	/* None tagged EDSA entry - place holder */
	mv_pp2x_prs_dsa_tag_set(pp2, 0, false, MVPP2_PRS_UNTAGGED,
			      MVPP2_PRS_EDSA);

	/* Tagged EDSA entry - place holder */
	mv_pp2x_prs_dsa_tag_set(pp2, 0, false,
				MVPP2_PRS_TAGGED, MVPP2_PRS_EDSA);

	/* None tagged DSA entry - place holder */
	mv_pp2x_prs_dsa_tag_set(pp2, 0, false, MVPP2_PRS_UNTAGGED,
			      MVPP2_PRS_DSA);

	/* Tagged DSA entry - place holder */
	mv_pp2x_prs_dsa_tag_set(pp2, 0, false, MVPP2_PRS_TAGGED, MVPP2_PRS_DSA);

	/* None tagged EDSA ethertype entry - place holder*/
	mv_pp2x_prs_dsa_tag_ethertype_set(pp2, 0, false,
					MVPP2_PRS_UNTAGGED, MVPP2_PRS_EDSA);

	/* Tagged EDSA ethertype entry - place holder*/
	mv_pp2x_prs_dsa_tag_ethertype_set(pp2, 0, false,
					MVPP2_PRS_TAGGED, MVPP2_PRS_EDSA);

	/* None tagged DSA ethertype entry */
	mv_pp2x_prs_dsa_tag_ethertype_set(pp2, 0, true,
					MVPP2_PRS_UNTAGGED, MVPP2_PRS_DSA);

	/* Tagged DSA ethertype entry */
	mv_pp2x_prs_dsa_tag_ethertype_set(pp2, 0, true,
					MVPP2_PRS_TAGGED, MVPP2_PRS_DSA);

	/* Set default entry, in case DSA or EDSA tag not found */
	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_DSA);
	pe.index = MVPP2_PE_DSA_DEFAULT;
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_VLAN);

	/* Shift 0 bytes */
	mv_pp2x_prs_sram_shift_set(&pe, 0, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_MAC);

	/* Clear all sram ai bits for next iteration */
	mv_pp2x_prs_sram_ai_update(&pe, 0, MVPP2_PRS_SRAM_AI_MASK);

	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	mv_pp2x_prs_hw_write(pp2, &pe);
}

/* Match basic ethertypes */
static int mv_pp2x_prs_etype_init(struct mv_pp2x *pp2)
{
	struct mv_pp2x_prs_entry pe;
	int tid;

	/* Ethertype: PPPoE */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = tid;

	mv_pp2x_prs_match_etype(&pe, 0, MV_PPPOE_TYPE);

	mv_pp2x_prs_sram_shift_set(&pe, MVPP2_PPPOE_HDR_SIZE,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_PPPOE);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_PPPOE_MASK,
				 MVPP2_PRS_RI_PPPOE_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_L2);
	pp2->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	pp2->prs_shadow[pe.index].finish = false;
	mv_pp2x_prs_shadow_ri_set(pp2, pe.index, MVPP2_PRS_RI_PPPOE_MASK,
				MVPP2_PRS_RI_PPPOE_MASK);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Ethertype: ARP */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = tid;

	mv_pp2x_prs_match_etype(&pe, 0, MV_IP_ARP_TYPE);

	/* Generate flow in the next iteration*/
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_ARP,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Set L3 offset */
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_L2);
	pp2->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	pp2->prs_shadow[pe.index].finish = true;
	mv_pp2x_prs_shadow_ri_set(pp2, pe.index, MVPP2_PRS_RI_L3_ARP,
				MVPP2_PRS_RI_L3_PROTO_MASK);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Ethertype: LBTD */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = tid;

	mv_pp2x_prs_match_etype(&pe, 0, MVPP2_IP_LBDT_TYPE);

	/* Generate flow in the next iteration*/
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_CPU_CODE_RX_SPEC |
				 MVPP2_PRS_RI_UDF3_RX_SPECIAL,
				 MVPP2_PRS_RI_CPU_CODE_MASK |
				 MVPP2_PRS_RI_UDF3_MASK);
	/* Set L3 offset */
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_L2);
	pp2->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	pp2->prs_shadow[pe.index].finish = true;
	mv_pp2x_prs_shadow_ri_set(pp2, pe.index, MVPP2_PRS_RI_CPU_CODE_RX_SPEC |
				MVPP2_PRS_RI_UDF3_RX_SPECIAL,
				MVPP2_PRS_RI_CPU_CODE_MASK |
				MVPP2_PRS_RI_UDF3_MASK);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Ethertype: IPv4 without options */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = tid;

	mv_pp2x_prs_match_etype(&pe, 0, MV_IP_TYPE);
	mv_pp2x_prs_tcam_data_byte_set(&pe, MVPP2_ETH_TYPE_LEN,
				     MVPP2_PRS_IPV4_HEAD | MVPP2_PRS_IPV4_IHL,
				     MVPP2_PRS_IPV4_HEAD_MASK |
				     MVPP2_PRS_IPV4_IHL_MASK);

	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP4,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Skip eth_type + 4 bytes of IP header */
	mv_pp2x_prs_sram_shift_set(&pe, MVPP2_ETH_TYPE_LEN + 4,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	/* Set L3 offset */
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_L2);
	pp2->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	pp2->prs_shadow[pe.index].finish = false;
	mv_pp2x_prs_shadow_ri_set(pp2, pe.index, MVPP2_PRS_RI_L3_IP4,
				MVPP2_PRS_RI_L3_PROTO_MASK);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Ethertype: IPv4 with options */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	pe.index = tid;

	/* Clear tcam data before updating */
	pe.tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE(MVPP2_ETH_TYPE_LEN)] = 0x0;
	pe.tcam.byte[MVPP2_PRS_TCAM_DATA_BYTE_EN(MVPP2_ETH_TYPE_LEN)] = 0x0;

	mv_pp2x_prs_tcam_data_byte_set(&pe, MVPP2_ETH_TYPE_LEN,
				     MVPP2_PRS_IPV4_HEAD,
				     MVPP2_PRS_IPV4_HEAD_MASK);

	/* Clear ri before updating */
	pe.sram.word[MVPP2_PRS_SRAM_RI_WORD] = 0x0;
	pe.sram.word[MVPP2_PRS_SRAM_RI_CTRL_WORD] = 0x0;
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP4_OPT,
				 MVPP2_PRS_RI_L3_PROTO_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_L2);
	pp2->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	pp2->prs_shadow[pe.index].finish = false;
	mv_pp2x_prs_shadow_ri_set(pp2, pe.index, MVPP2_PRS_RI_L3_IP4_OPT,
				MVPP2_PRS_RI_L3_PROTO_MASK);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Ethertype: IPv6 without options */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = tid;

	mv_pp2x_prs_match_etype(&pe, 0, MV_IP6_TYPE);

	/* Skip DIP of IPV6 header */
	mv_pp2x_prs_sram_shift_set(&pe, MVPP2_ETH_TYPE_LEN + 8 +
				 MVPP2_MAX_L3_ADDR_SIZE,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP6);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP6,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Set L3 offset */
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_L2);
	pp2->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	pp2->prs_shadow[pe.index].finish = false;
	mv_pp2x_prs_shadow_ri_set(pp2, pe.index, MVPP2_PRS_RI_L3_IP6,
				MVPP2_PRS_RI_L3_PROTO_MASK);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Default entry for MVPP2_PRS_LU_L2 - Unknown ethtype */
	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_L2);
	pe.index = MVPP2_PE_ETH_TYPE_UN;

	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Generate flow in the next iteration*/
	mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_UN,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Set L3 offset even it's unknown L3 */
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_L2);
	pp2->prs_shadow[pe.index].udf = MVPP2_PRS_UDF_L2_DEF;
	pp2->prs_shadow[pe.index].finish = true;
	mv_pp2x_prs_shadow_ri_set(pp2, pe.index, MVPP2_PRS_RI_L3_UN,
				MVPP2_PRS_RI_L3_PROTO_MASK);
	mv_pp2x_prs_hw_write(pp2, &pe);

	return 0;
}

/* Configure vlan entries and detect up to 2 successive VLAN tags.
 * Possible options:
 * 0x8100, 0x88A8
 * 0x8100, 0x8100
 * 0x8100
 * 0x88A8
 */
static int mv_pp2x_prs_vlan_init(struct mv_pp2x *pp2)
{
	struct mv_pp2x_prs_entry pe;
	int err;

	pp2->prs_double_vlans = calloc(1, sizeof(bool));
	if (!pp2->prs_double_vlans)
		return -ENOMEM;

	/* Double VLAN: 0x8100, 0x88A8 */
	err = mv_pp2x_prs_double_vlan_add(pp2, MV_VLAN_TYPE, MV_VLAN_1_TYPE,
					MVPP2_PRS_PORT_MASK);
	if (err)
		return err;

	/* Double VLAN: 0x8100, 0x8100 */
	err = mv_pp2x_prs_double_vlan_add(pp2, MV_VLAN_TYPE, MV_VLAN_TYPE,
					MVPP2_PRS_PORT_MASK);
	if (err)
		return err;

	/* Single VLAN: 0x88a8 */
	err = mv_pp2x_prs_vlan_add(pp2, MV_VLAN_1_TYPE,
			MVPP2_PRS_SINGLE_VLAN_AI, MVPP2_PRS_PORT_MASK);
	if (err)
		return err;

	/* Single VLAN: 0x8100 */
	err = mv_pp2x_prs_vlan_add(pp2, MV_VLAN_TYPE, MVPP2_PRS_SINGLE_VLAN_AI,
				 MVPP2_PRS_PORT_MASK);
	if (err)
		return err;

	/* Set default double vlan entry */
	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_VLAN);
	pe.index = MVPP2_PE_VLAN_DBL;

	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_L2);
	/* Clear ai for next iterations */
	mv_pp2x_prs_sram_ai_update(&pe, 0, MVPP2_PRS_SRAM_AI_MASK);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_DOUBLE,
				 MVPP2_PRS_RI_VLAN_MASK);

	mv_pp2x_prs_tcam_ai_update(&pe, MVPP2_PRS_DBL_VLAN_AI_BIT,
				 MVPP2_PRS_DBL_VLAN_AI_BIT);
	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_VLAN);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Set default vlan none entry */
	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_VLAN);
	pe.index = MVPP2_PE_VLAN_NONE;

	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_L2);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_VLAN_NONE,
				 MVPP2_PRS_RI_VLAN_MASK);

	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_VLAN);
	mv_pp2x_prs_hw_write(pp2, &pe);

	return 0;
}

/* Set entries for PPPoE ethertype */
static int mv_pp2x_prs_pppoe_init(struct mv_pp2x *pp2)
{
	struct mv_pp2x_prs_entry pe;
	int tid;

	/* IPv4 over PPPoE with options */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_PPPOE);
	pe.index = tid;

	mv_pp2x_prs_match_etype(&pe, 0, MV_PPP_IP_TYPE);

	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP4_OPT,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Skip eth_type + 4 bytes of IP header */
	mv_pp2x_prs_sram_shift_set(&pe, MVPP2_ETH_TYPE_LEN + 4,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	/* Set L3 offset */
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_PPPOE);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* IPv4 over PPPoE without options */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	pe.index = tid;

	mv_pp2x_prs_tcam_data_byte_set(&pe, MVPP2_ETH_TYPE_LEN,
				     MVPP2_PRS_IPV4_HEAD | MVPP2_PRS_IPV4_IHL,
				     MVPP2_PRS_IPV4_HEAD_MASK |
				     MVPP2_PRS_IPV4_IHL_MASK);

	/* Clear ri before updating */
	pe.sram.word[MVPP2_PRS_SRAM_RI_WORD] = 0x0;
	pe.sram.word[MVPP2_PRS_SRAM_RI_CTRL_WORD] = 0x0;
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP4,
				 MVPP2_PRS_RI_L3_PROTO_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_PPPOE);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* IPv6 over PPPoE */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_PPPOE);
	pe.index = tid;

	mv_pp2x_prs_match_etype(&pe, 0, MV_PPP_IP6_TYPE);

	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP6);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_IP6,
				 MVPP2_PRS_RI_L3_PROTO_MASK);
	/* Skip eth_type + 4 bytes of IPv6 header */
	mv_pp2x_prs_sram_shift_set(&pe, MVPP2_ETH_TYPE_LEN + 4,
				 MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	/* Set L3 offset */
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_PPPOE);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Non-IP over PPPoE */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_PPPOE);
	pe.index = tid;

	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_UN,
				 MVPP2_PRS_RI_L3_PROTO_MASK);

	/* Finished: go to flowid generation */
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	/* Set L3 offset even if it's unknown L3 */
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L3,
				  MVPP2_ETH_TYPE_LEN,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_PPPOE);
	mv_pp2x_prs_hw_write(pp2, &pe);

	return 0;
}

/* Initialize entries for IPv4 */
static int mv_pp2x_prs_ip4_init(struct mv_pp2x *pp2)
{
	struct mv_pp2x_prs_entry pe;
	int err;

	/* Set entries for TCP, UDP and IGMP over IPv4 */
	err = mv_pp2x_prs_ip4_proto(pp2, 6/*IPPROTO_TCP*/, MVPP2_PRS_RI_L4_TCP,
				  MVPP2_PRS_RI_L4_PROTO_MASK);
	if (err)
		return err;

	err = mv_pp2x_prs_ip4_proto(pp2, IPPROTO_UDP, MVPP2_PRS_RI_L4_UDP,
				  MVPP2_PRS_RI_L4_PROTO_MASK);
	if (err)
		return err;

	err = mv_pp2x_prs_ip4_proto(pp2, 2/*IPPROTO_IGMP*/,
				  MVPP2_PRS_RI_CPU_CODE_RX_SPEC |
				  MVPP2_PRS_RI_UDF3_RX_SPECIAL,
				  MVPP2_PRS_RI_CPU_CODE_MASK |
				  MVPP2_PRS_RI_UDF3_MASK);
	if (err)
		return err;

	/* IPv4 Broadcast */
	err = mv_pp2x_prs_ip4_cast(pp2, MVPP2_PRS_L3_BROAD_CAST);
	if (err)
		return err;

	/* IPv4 Multicast */
	err = mv_pp2x_prs_ip4_cast(pp2, MVPP2_PRS_L3_MULTI_CAST);
	if (err)
		return err;

	/* Default IPv4 entry for unknown protocols */
	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP4);
	pe.index = MVPP2_PE_IP4_PROTO_UN;

	/* Set next lu to IPv4 */
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_sram_shift_set(&pe, 12, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);
	/* Set L4 offset */
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L4,
				  sizeof(struct ip_hdr) - 4,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);
	mv_pp2x_prs_sram_ai_update(&pe, MVPP2_PRS_IPV4_DIP_AI_BIT,
				 MVPP2_PRS_IPV4_DIP_AI_BIT);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L4_OTHER,
				 MVPP2_PRS_RI_L4_PROTO_MASK);

	mv_pp2x_prs_tcam_ai_update(&pe, 0, MVPP2_PRS_IPV4_DIP_AI_BIT);
	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Default IPv4 entry for unicast address */
	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP4);
	pe.index = MVPP2_PE_IP4_ADDR_UN;

	/* Finished: go to flowid generation */
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_UCAST,
				 MVPP2_PRS_RI_L3_ADDR_MASK);

	mv_pp2x_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV4_DIP_AI_BIT,
				 MVPP2_PRS_IPV4_DIP_AI_BIT);
	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_hw_write(pp2, &pe);

	return 0;
}

/* Initialize entries for IPv6 */
static int mv_pp2x_prs_ip6_init(struct mv_pp2x *pp2)
{
	struct mv_pp2x_prs_entry pe;
	int tid, err;

	/* Set entries for TCP, UDP and ICMP over IPv6 */
	err = mv_pp2x_prs_ip6_proto(pp2, 6/*IPPROTO_TCP*/,
				  MVPP2_PRS_RI_L4_TCP,
				  MVPP2_PRS_RI_L4_PROTO_MASK);
	if (err)
		return err;

	err = mv_pp2x_prs_ip6_proto(pp2, IPPROTO_UDP,
				  MVPP2_PRS_RI_L4_UDP,
				  MVPP2_PRS_RI_L4_PROTO_MASK);
	if (err)
		return err;

	err = mv_pp2x_prs_ip6_proto(pp2, 58/*IPPROTO_ICMPV6*/,
				  MVPP2_PRS_RI_CPU_CODE_RX_SPEC |
				  MVPP2_PRS_RI_UDF3_RX_SPECIAL,
				  MVPP2_PRS_RI_CPU_CODE_MASK |
				  MVPP2_PRS_RI_UDF3_MASK);
	if (err)
		return err;

	/* IPv4 is the last header. This is similar case as 6-TCP or 17-UDP */
	/* Result Info: UDF7=1, DS lite */
	err = mv_pp2x_prs_ip6_proto(pp2, 4/*IPPROTO_IPIP*/,
				  MVPP2_PRS_RI_UDF7_IP6_LITE,
				  MVPP2_PRS_RI_UDF7_MASK);
	if (err)
		return err;

	/* IPv6 multicast */
	err = mv_pp2x_prs_ip6_cast(pp2, MVPP2_PRS_L3_MULTI_CAST);
	if (err)
		return err;

	/* Entry for checking hop limit */
	tid = mv_pp2x_prs_tcam_first_free(pp2, MVPP2_PE_FIRST_FREE_TID,
					MVPP2_PE_LAST_FREE_TID);
	if (tid < 0)
		return tid;

	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = tid;

	/* Finished: go to flowid generation */
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_UN |
				 MVPP2_PRS_RI_DROP_MASK,
				 MVPP2_PRS_RI_L3_PROTO_MASK |
				 MVPP2_PRS_RI_DROP_MASK);

	mv_pp2x_prs_tcam_data_byte_set(&pe, 1, 0x00, MVPP2_PRS_IPV6_HOP_MASK);
	mv_pp2x_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV6_NO_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_NO_EXT_AI_BIT);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Default IPv6 entry for unknown protocols */
	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = MVPP2_PE_IP6_PROTO_UN;

	/* Finished: go to flowid generation */
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L4_OTHER,
				 MVPP2_PRS_RI_L4_PROTO_MASK);
	/* Set L4 offset relatively to our current place */
	mv_pp2x_prs_sram_offset_set(&pe, MVPP2_PRS_SRAM_UDF_TYPE_L4,
				  /*sizeof(struct ipv6hdr)*/40 - 4,
				  MVPP2_PRS_SRAM_OP_SEL_UDF_ADD);

	mv_pp2x_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV6_NO_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Default IPv6 entry for unknown ext protocols */
	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = MVPP2_PE_IP6_EXT_PROTO_UN;

	/* Finished: go to flowid generation */
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_FLOWS);
	mv_pp2x_prs_sram_bits_set(&pe, MVPP2_PRS_SRAM_LU_GEN_BIT, 1);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L4_OTHER,
				 MVPP2_PRS_RI_L4_PROTO_MASK);

	mv_pp2x_prs_tcam_ai_update(&pe, MVPP2_PRS_IPV6_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_EXT_AI_BIT);
	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_IP4);
	mv_pp2x_prs_hw_write(pp2, &pe);

	/* Default IPv6 entry for unicast address */
	memset(&pe, 0, sizeof(struct mv_pp2x_prs_entry));
	mv_pp2x_prs_tcam_lu_set(&pe, MVPP2_PRS_LU_IP6);
	pe.index = MVPP2_PE_IP6_ADDR_UN;

	/* Finished: go to IPv6 again */
	mv_pp2x_prs_sram_next_lu_set(&pe, MVPP2_PRS_LU_IP6);
	mv_pp2x_prs_sram_ri_update(&pe, MVPP2_PRS_RI_L3_UCAST,
				 MVPP2_PRS_RI_L3_ADDR_MASK);
	mv_pp2x_prs_sram_ai_update(&pe, MVPP2_PRS_IPV6_NO_EXT_AI_BIT,
				 MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Shift back to IPV6 NH */
	mv_pp2x_prs_sram_shift_set(&pe, -18, MVPP2_PRS_SRAM_OP_SEL_SHIFT_ADD);

	mv_pp2x_prs_tcam_ai_update(&pe, 0, MVPP2_PRS_IPV6_NO_EXT_AI_BIT);
	/* Unmask all ports */
	mv_pp2x_prs_tcam_port_map_set(&pe, MVPP2_PRS_PORT_MASK);

	/* Update shadow table and hw entry */
	mv_pp2x_prs_shadow_set(pp2, pe.index, MVPP2_PRS_LU_IP6);
	mv_pp2x_prs_hw_write(pp2, &pe);

	return 0;
}

int mv_pp2x_prs_tag_mode_set(struct mv_pp2x_port *pp, int type)
{
	switch (type) {
	case MVPP2_TAG_TYPE_EDSA:
		/* Add port to EDSA entries */
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, true,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_EDSA);
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, true,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_EDSA);
		/* Remove port from DSA entries */
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, false,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_DSA);
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, false,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_DSA);
		break;

	case MVPP2_TAG_TYPE_DSA:
		/* Add port to DSA entries */
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, true,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_DSA);
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, true,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_DSA);
		/* Remove port from EDSA entries */
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, false,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_EDSA);
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, false,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_EDSA);
		break;

	case MVPP2_TAG_TYPE_MH:
	case MVPP2_TAG_TYPE_NONE:
		/* Remove port form EDSA and DSA entries */
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, false,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_DSA);
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, false,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_DSA);
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, false,
				      MVPP2_PRS_TAGGED, MVPP2_PRS_EDSA);
		mv_pp2x_prs_dsa_tag_set(pp->pp2, pp->id, false,
				      MVPP2_PRS_UNTAGGED, MVPP2_PRS_EDSA);
		break;

	default:
		if ((type < 0) || (type > MVPP2_TAG_TYPE_EDSA))
			return -EINVAL;
	}

	return 0;
}

/* Find parser flow entry */
static struct mv_pp2x_prs_entry *mv_pp2x_prs_flow_find(struct mv_pp2x *pp2,
				int flow, unsigned int ri, unsigned int ri_mask)
{
	struct mv_pp2x_prs_entry *pe;
	int tid;
	unsigned int dword, enable;

	pe = kzalloc(sizeof(*pe), GFP_KERNEL);
	if (!pe)
		return NULL;
	mv_pp2x_prs_tcam_lu_set(pe, MVPP2_PRS_LU_FLOWS);

	/* Go through the all entires with MVPP2_PRS_LU_FLOWS */
	for (tid = MVPP2_PRS_TCAM_SRAM_SIZE - 1; tid >= 0; tid--) {
		u8 bits;

		if (!pp2->prs_shadow[tid].valid ||
		    pp2->prs_shadow[tid].lu != MVPP2_PRS_LU_FLOWS)
			continue;

		pe->index = tid;
		mv_pp2x_prs_hw_read(pp2, pe);

		/* Check result info, because there maybe
		*   several TCAM lines to generate the same flow */
		mv_pp2x_prs_tcam_data_dword_get(pe, 0, &dword, &enable);
		if ((dword != ri) || (enable != ri_mask))
			continue;

		bits = mv_pp2x_prs_sram_ai_get(pe);

		/* Sram store classification lookup ID in AI bits [5:0] */
		if ((bits & MVPP2_PRS_FLOW_ID_MASK) == flow)
			return pe;
	}
	kfree(pe);

	return NULL;
}

/* Set prs flow for the port */
int mv_pp2x_prs_def_flow(struct mv_pp2x_port *port)
{
	struct mv_pp2x_prs_entry *pe;
	int tid;

	pe = mv_pp2x_prs_flow_find(port->pp2, port->id, 0, 0);

	/* Such entry not exist */
	if (!pe) {
		/* Go through the all entires from last to first */
		tid = mv_pp2x_prs_tcam_first_free(port->pp2,
						MVPP2_PE_LAST_FREE_TID,
					       MVPP2_PE_FIRST_FREE_TID);
		if (tid < 0)
			return tid;

		pe = kzalloc(sizeof(*pe), GFP_KERNEL);
		if (!pe)
			return -ENOMEM;

		mv_pp2x_prs_tcam_lu_set(pe, MVPP2_PRS_LU_FLOWS);
		pe->index = tid;

		/* Set flow ID*/
		mv_pp2x_prs_sram_ai_update(pe, port->id, MVPP2_PRS_FLOW_ID_MASK);
		mv_pp2x_prs_sram_bits_set(pe, MVPP2_PRS_SRAM_LU_DONE_BIT, 1);

		/* Update shadow table */
		mv_pp2x_prs_shadow_set(port->pp2, pe->index, MVPP2_PRS_LU_FLOWS);
	}

	mv_pp2x_prs_tcam_port_map_set(pe, (1 << port->id));
	mv_pp2x_prs_hw_write(port->pp2, pe);
	kfree(pe);

	return 0;
}

/* Parser default initialization */
int mv_pp2x_prs_default_init(struct mv_pp2x *pp2)
{
	int err, index, i;

	/* Enable tcam table */
	mv_pp2x_write(pp2, MVPP2_PRS_TCAM_CTRL_REG, MVPP2_PRS_TCAM_EN_MASK);

	/* Clear all tcam and sram entries */
	for (index = 0; index < MVPP2_PRS_TCAM_SRAM_SIZE; index++) {
		mv_pp2x_write(pp2, MVPP2_PRS_TCAM_IDX_REG, index);
		for (i = 0; i < MVPP2_PRS_TCAM_WORDS; i++)
			mv_pp2x_write(pp2, MVPP2_PRS_TCAM_DATA_REG(i), 0);

		mv_pp2x_write(pp2, MVPP2_PRS_SRAM_IDX_REG, index);
		for (i = 0; i < MVPP2_PRS_SRAM_WORDS; i++)
			mv_pp2x_write(pp2, MVPP2_PRS_SRAM_DATA_REG(i), 0);
	}

	/* Invalidate all tcam entries */
	for (index = 0; index < MVPP2_PRS_TCAM_SRAM_SIZE; index++)
		mv_pp2x_prs_hw_inv(pp2, index);

	pp2->prs_shadow = calloc(MVPP2_PRS_TCAM_SRAM_SIZE,
				sizeof(struct mv_pp2x_prs_shadow));
	if (!pp2->prs_shadow)
		return -ENOMEM;

	/* Always start from lookup = 0 */
	for (index = 0; index < CONFIG_MAX_PP2_PORT_NUM; index++)
		mv_pp2x_prs_hw_port_init(pp2, index, MVPP2_PRS_LU_MH,
					MVPP2_PRS_PORT_LU_MAX, 0);

	/* Always start from lookup = 0 */
	mv_pp2x_prs_def_flow_init(pp2);

	mv_pp2x_prs_mh_init(pp2);

	mv_pp2x_prs_mac_init(pp2);

	mv_pp2x_prs_dsa_init(pp2);

	err = mv_pp2x_prs_etype_init(pp2);
	if (err)
		return err;

	err = mv_pp2x_prs_vlan_init(pp2);
	if (err)
		return err;

	err = mv_pp2x_prs_pppoe_init(pp2);
	if (err)
		return err;

	err = mv_pp2x_prs_ip6_init(pp2);
	if (err)
		return err;

	err = mv_pp2x_prs_ip4_init(pp2);
	if (err)
		return err;

	return 0;
}

/* Update classification flow table registers */
static void mv_pp2x_cls_hw_flow_write(struct mv_pp2x *pp2,
				    struct mv_pp2x_cls_flow_entry *fe)
{
	/* Write to index reg - indirect access */
	mv_pp2x_write(pp2, MVPP2_CLS_FLOW_INDEX_REG, fe->index);

	mv_pp2x_write(pp2, MVPP2_CLS_FLOW_TBL0_REG, fe->data[0]);
	mv_pp2x_write(pp2, MVPP2_CLS_FLOW_TBL1_REG, fe->data[1]);
	mv_pp2x_write(pp2, MVPP2_CLS_FLOW_TBL2_REG, fe->data[2]);
}

/* Update classification lookup table register */
static void mv_pp2x_cls_hw_lkp_write(struct mv_pp2x *pp2,
				   struct mv_pp2x_cls_lkp_entry *le)
{
	u32 reg_val = 0;

	/* Write to index reg - indirect access */
	reg_val = (le->way << MVPP2_CLS_LKP_INDEX_WAY_OFFS) | le->lkpid;
	mv_pp2x_write(pp2, MVPP2_CLS_LKP_INDEX_REG, reg_val);

	mv_pp2x_write(pp2, MVPP2_CLS_LKP_TBL_REG, le->data);
}

/* Classifier default initialization */
int mv_pp2x_cls_default_init(struct mv_pp2x *pp2)
{
	struct mv_pp2x_cls_lkp_entry le;
	struct mv_pp2x_cls_flow_entry fe;
	int index;

	/* Enable Classifier */
	mv_pp2x_write(pp2, MVPP2_CLS_MODE_REG, MVPP2_CLS_MODE_ACTIVE_MASK);

	/* Clear cls flow table */
	fe.data[0] = 0;
	fe.data[1] = 0;
	fe.data[2] = 0;
	for (index = 0; index < MVPP2_CLS_FLOWS_TBL_SIZE; index++) {
		fe.index = index;
		mv_pp2x_cls_hw_flow_write(pp2, &fe);
	}

	/* Clear cls lookup table */
	le.data = 0;
	for (index = 0; index < MVPP2_CLS_LKP_TBL_SIZE; index++) {
		le.lkpid = index;
		le.way = 0;
		mv_pp2x_cls_hw_lkp_write(pp2, &le);
		le.way = 1;
		mv_pp2x_cls_hw_lkp_write(pp2, &le);
	}

	return 0;
}

static void mv_pp2x_cls_port_default_config(struct mv_pp2x_port *port)
{
	struct mv_pp2x_cls_lkp_entry le;
	u32 val;

	/* Set way for the port */
	val = mv_pp2x_read(port->pp2, MVPP2_CLS_PORT_WAY_REG);
	val &= ~MVPP2_CLS_PORT_WAY_MASK(port->id);
	mv_pp2x_write(port->pp2, MVPP2_CLS_PORT_WAY_REG, val);

	/* Pick the entry to be accessed in lookup ID decoding table
	 * according to the way and lkpid.
	 */
	le.lkpid = port->id;
	le.way = 0;
	le.data = 0;

	/* Set initial CPU queue for receiving packets */
	le.data &= ~MVPP2_CLS_LKP_TBL_RXQ_MASK;
	le.data |= port->first_rxq;

	/* Disable classification engines */
	le.data &= ~MVPP2_CLS_LKP_TBL_LOOKUP_EN_MASK;

	/* Update lookup ID table entry */
	mv_pp2x_cls_hw_lkp_write(port->pp2, &le);
}

/* Set CPU queue number for oversize packets */
void mv_pp2x_cls_oversize_rxq_set(struct mv_pp2x_port *port)
{
	mv_pp2x_write(port->pp2, MVPP2_CLS_OVERSIZE_RXQ_LOW_REG(port->id),
		    port->first_rxq & MVPP2_CLS_OVERSIZE_RXQ_LOW_MASK);
}

/* Initialize Rx FIFO's */
static void mv_pp2x_rx_fifo_init(struct mv_pp2x *pp2)
{
	int port;

	for (port = 0; port < CONFIG_MAX_PP2_PORT_NUM; port++) {
		mv_pp2x_write(pp2, MVPP2_RX_DATA_FIFO_SIZE_REG(port),
			    MVPP2_RX_FIFO_PORT_DATA_SIZE);
		mv_pp2x_write(pp2, MVPP2_RX_ATTR_FIFO_SIZE_REG(port),
			    MVPP2_RX_FIFO_PORT_ATTR_SIZE);
	}

	mv_pp2x_write(pp2, MVPP2_RX_MIN_PKT_SIZE_REG,
		    MVPP2_RX_FIFO_PORT_MIN_PKT);
	mv_pp2x_write(pp2, MVPP2_RX_FIFO_INIT_REG, 0x1);
}

/* BM */
static int mv_pp2x_bm_pool_ctrl(struct mv_pp2x *pp2, int pool, enum mv_pp2x_command cmd)
{
	u32 reg_val = 0;
	reg_val = mv_pp2x_read(pp2, MVPP2_BM_POOL_CTRL_REG(pool));

	switch (cmd) {
	case MVPP2_START:
		reg_val |= MVPP2_BM_START_MASK;
		break;

	case MVPP2_STOP:
		reg_val |= MVPP2_BM_STOP_MASK;
		break;

	default:
		return -ENOMEM;
	}
	mv_pp2x_write(pp2, MVPP2_BM_POOL_CTRL_REG(pool), reg_val);

	return 0;
}

static int mv_pp2x_bm_pool_buf_size_set(struct mv_pp2x *pp2, int pool, int bufsize)
{
	u32 reg_val;

	pp2->bm_pools->buf_size = bufsize;
	reg_val = ALIGN_UP(bufsize, 1 << MVPP2_POOL_BUF_SIZE_OFFSET);
	mv_pp2x_write(pp2, MVPP2_POOL_BUF_SIZE_REG(pool), reg_val);

	return 0;
}

static void mv_pp2x_bm_hw_pool_create(struct mv_pp2x *pp2)
{
	mv_pp2x_write(pp2, MVPP2_BM_POOL_BASE_REG(pp2->bm_pools->id),
				lower_32_bits(pp2->bm_pools->phys_addr));

	mv_pp2x_write(pp2, MVPP22_BM_POOL_BASE_HIGH_REG,
	(upper_32_bits(pp2->bm_pools->phys_addr)&
			MVPP22_BM_POOL_BASE_HIGH_REG));
	mv_pp2x_write(pp2, MVPP2_BM_POOL_SIZE_REG(pp2->bm_pools->id),
					pp2->bm_pools->size);

}

/*
 * mv_pp2x_bm_pool_init
 *   initialize BM pool to be used by all ports 
 */

static int mv_pp2x_bm_pool_init(struct mv_pp2x *pp2)
{
	int i;
	unsigned char *pool_addr;

	for (i = 0; i < MVPP2_BM_POOLS_NUM; i++) {
		/* Mask BM all interrupts */
		mv_pp2x_write(pp2, MVPP2_BM_INTR_MASK_REG(i), 0);
		/* Clear BM cause register */
		mv_pp2x_write(pp2, MVPP2_BM_INTR_CAUSE_REG(i), 0);
	}

	pp2->bm_pools = (struct mv_pp2x_bm_pool *)calloc(1,
					sizeof(struct mv_pp2x_bm_pool));
	if (!pp2->bm_pools)
		return -1;

	pool_addr = malloc((sizeof(uintptr_t) * MVPP2_BM_SIZE)*2 +
			MVPP2_BM_POOL_PTR_ALIGN);

	if (!pool_addr) {
		printf("Can't alloc %ld bytes for pool #%d\n",
			sizeof(u32) * MVPP2_BM_SIZE, MVPP2_BM_POOL);
		return -1;
	}
	if (IS_NOT_ALIGN((unsigned long)pool_addr,
		MVPP2_BM_POOL_PTR_ALIGN))
		pool_addr =
		(unsigned char *)ALIGN_UP((unsigned long)pool_addr,
						MVPP2_BM_POOL_PTR_ALIGN);

	pp2->bm_pools->id = MVPP2_BM_POOL;
	pp2->bm_pools->virt_addr = pool_addr;
	pp2->bm_pools->phys_addr = (unsigned long)pool_addr;
	pp2->bm_pools->size = MVPP2_BM_SIZE;

	mv_pp2x_bm_hw_pool_create(pp2);

	return 0;
}

static void mv_pp2x_bm_pool_put(struct mv_pp2x *pp2, int pool,
			unsigned long bufPhysAddr, unsigned long bufVirtAddr)
{
#ifdef CONFIG_MVPPV22
	u32 val = 0;

	val = (upper_32_bits((uintptr_t)bufVirtAddr) & MVPP22_ADDR_HIGH_MASK)
		<< MVPP22_BM_VIRT_HIGH_RLS_OFFST;
	val |= (upper_32_bits(bufPhysAddr) & MVPP22_ADDR_HIGH_MASK)
		<< MVPP22_BM_PHY_HIGH_RLS_OFFSET;
	mv_pp2x_write(pp2, MVPP22_BM_PHY_VIRT_HIGH_RLS_REG, val);
#endif
	mv_pp2x_write(pp2, MVPP2_BM_VIRT_RLS_REG, (u32)bufVirtAddr);
	mv_pp2x_write(pp2, MVPP2_BM_PHY_RLS_REG(pool), (u32)bufPhysAddr);
}

/*
 * mv_pp2x_bm_start
 *   enable and fill BM pool
 */
static int mv_pp2x_bm_start(struct mv_pp2x *pp2)
{
	unsigned char *buff, *buff_phys;
	int i;

	mv_pp2x_bm_pool_ctrl(pp2, MVPP2_BM_POOL, MVPP2_START);

	mv_pp2x_bm_pool_buf_size_set(pp2, MVPP2_BM_POOL, RX_BUFFER_SIZE);


	/* fill BM pool with buffers */
	for (i = 0; i < MVPP2_BM_SIZE; i++) {
		buff = (unsigned char *)(buffer_loc.rx_buffers
			+ (i * RX_BUFFER_SIZE));
		if (!buff)
			return -1;

		buff_phys = (unsigned char *)ALIGN_UP((unsigned long)buff,
			BM_ALIGN);
		mv_pp2x_bm_pool_put(pp2, MVPP2_BM_POOL,
			(unsigned long)buff_phys, (unsigned long)buff_phys);
	}

	return 0;
}

/*
 * mv_pp2x_bm_stop
 * empty BM pool and stop its activity
 */
static void mv_pp2x_bm_stop(struct mv_pp2x *pp2)
{
	int i;

	for (i = 0; i < MVPP2_BM_SIZE; i++)
		mv_pp2x_read(pp2, MVPP2_BM_PHY_ALLOC_REG(0));

	mv_pp2x_bm_pool_ctrl(pp2, MVPP2_BM_POOL, MVPP2_STOP);

}

/* Port configuration routines */

void mv_pp2x_port_mii_set(struct mv_pp2x_port *port)
{
	u32 val;

	val = readl(port->base + MVPP2_GMAC_CTRL_2_REG);

	switch (port->mac_data.phy_mode) {
	case PHY_INTERFACE_MODE_SGMII:
		val |= MVPP2_GMAC_INBAND_AN_MASK;
		break;
	case PHY_INTERFACE_MODE_RGMII:
		val |= MVPP2_GMAC_PORT_RGMII_MASK;
	default:
		val &= ~MVPP2_GMAC_PCS_ENABLE_MASK;
	}

	writel(val, port->base + MVPP2_GMAC_CTRL_2_REG);
}

void mv_pp2x_port_fc_adv_enable(struct mv_pp2x_port *port)
{
	u32 val;

	val = readl(port->base + MVPP2_GMAC_AUTONEG_CONFIG);
	val |= MVPP2_GMAC_FC_ADV_EN;
	writel(val, port->base + MVPP2_GMAC_AUTONEG_CONFIG);
}

void mv_pp2x_port_enable(struct mv_pp2x_port *port)
{
	u32 val;

	val = readl(port->base + MVPP2_GMAC_CTRL_0_REG);
	val |= MVPP2_GMAC_PORT_EN_MASK;
	val |= MVPP2_GMAC_MIB_CNTR_EN_MASK;
	writel(val, port->base + MVPP2_GMAC_CTRL_0_REG);
}

void mv_pp2x_port_disable(struct mv_pp2x_port *port)
{
	u32 val;

	val = readl(port->base + MVPP2_GMAC_CTRL_0_REG);
	val &= ~(MVPP2_GMAC_PORT_EN_MASK);
	writel(val, port->base + MVPP2_GMAC_CTRL_0_REG);
}

/* Enable/disable receiving packets */
static void mv_pp2x_ingress_enable(struct mv_pp2x_port *pp, bool en)
{
	u32 reg_val;
	int lrxq, queue;

	for (lrxq = 0; lrxq < rxq_number; lrxq++) {
		queue = pp->rxqs[lrxq].id;
		reg_val = mv_pp2x_read(pp->pp2, MVPP2_RXQ_CONFIG_REG(queue));
		if (en)
			reg_val &= ~MVPP2_RXQ_DISABLE_MASK;
		else
			reg_val |= MVPP2_RXQ_DISABLE_MASK;
		mv_pp2x_write(pp->pp2, MVPP2_RXQ_CONFIG_REG(queue), reg_val);
	}
}

#ifndef CONFIG_MVPP2_FPGA
/* PP2 GOP/GMAC config */

/* GOP Functions */
static inline u32 mv_gop_gen_read(void __iomem *base, u32 offset)
{
	void *reg_ptr = base + offset;
	u32 val;

	val = readl(reg_ptr);
	return val;
}

static inline void mv_gop_gen_write(void __iomem *base, u32 offset, u32 data)
{
	void *reg_ptr = base + offset;

	writel(data, reg_ptr);
}

/* GMAC Functions  */
static inline u32 mv_gop110_gmac_read(struct gop_hw *gop,
				int mac_num, u32 offset)
{
	return mv_gop_gen_read(gop->gop_110.gmac.base,
		mac_num*gop->gop_110.gmac.obj_size + offset);
}

static inline void mv_gop110_gmac_write(struct gop_hw *gop,
				int mac_num, u32 offset, u32 data)
{
	mv_gop_gen_write(gop->gop_110.gmac.base,
			mac_num*gop->gop_110.gmac.obj_size + offset, data);
}

/* XLG MAC Functions */
static inline u32 mv_gop110_xlg_mac_read(struct gop_hw *gop,
					int mac_num, u32 offset)
{
	return mv_gop_gen_read(gop->gop_110.xlg_mac.base,
		mac_num*gop->gop_110.xlg_mac.obj_size + offset);
}

static inline void mv_gop110_xlg_mac_write(struct gop_hw *gop,
	int mac_num, u32 offset, u32 data)
{
	mv_gop_gen_write(gop->gop_110.xlg_mac.base,
		mac_num*gop->gop_110.xlg_mac.obj_size + offset, data);
}

/* Serdes Functions */
static inline u32 mv_gop110_serdes_read(struct gop_hw *gop,
					int lane_num, u32 offset)
{
	return mv_gop_gen_read(gop->gop_110.serdes.base,
		lane_num*gop->gop_110.serdes.obj_size + offset);
}

static inline void mv_gop110_serdes_write(struct gop_hw *gop,
					int lane_num, u32 offset, u32 data)
{
	mv_gop_gen_write(gop->gop_110.serdes.base,
		lane_num * gop->gop_110.serdes.obj_size + offset, data);
}

/* XPCS Functions */

static inline u32 mv_gop110_xpcs_global_read(struct gop_hw *gop, u32 offset)
{
	return mv_gop_gen_read(gop->gop_110.xpcs_base, offset);
}
static inline void mv_gop110_xpcs_global_write(struct gop_hw *gop,
					u32 offset, u32 data)
{
	mv_gop_gen_write(gop->gop_110.xpcs_base, offset, data);
}

/* GOP SMI Functions  */
static inline u32 mv_gop110_smi_read(struct gop_hw *gop, u32 offset)
{
	return mv_gop_gen_read(gop->gop_110.smi_base, offset);
}

static inline void mv_gop110_smi_write(struct gop_hw *gop, u32 offset, u32 data)
{
	mv_gop_gen_write(gop->gop_110.smi_base, offset, data);
}

/*
* mv_gop_phy_addr_cfg
*/
int mv_gop110_smi_phy_addr_cfg(struct gop_hw *gop, int port, int addr)
{
	mv_gop110_smi_write(gop, MV_SMI_PHY_ADDRESS_REG(port), addr);

	return 0;
}

/* Set the MAC to reset or exit from reset */
int mv_gop110_gmac_reset(struct gop_hw *gop, int mac_num, enum mv_reset reset)
{
	u32 reg_addr;
	u32 val;

	reg_addr = MV_GMAC_PORT_CTRL2_REG;

	/* read - modify - write */
	val = mv_gop110_gmac_read(gop, mac_num, reg_addr);
	if (reset == RESET)
		val |= MV_GMAC_PORT_CTRL2_PORTMACRESET_MASK;
	else
		val &= ~MV_GMAC_PORT_CTRL2_PORTMACRESET_MASK;
	mv_gop110_gmac_write(gop, mac_num, reg_addr, val);

	return 0;
}

/*
* mv_gop110_gpcs_mode_cfg
*Configure port to working with Gig PCS or don't.
*/
int mv_gop110_gpcs_mode_cfg(struct gop_hw *gop, int pcs_num, bool en)
{
	u32 val;

	val = mv_gop110_gmac_read(gop, pcs_num, MV_GMAC_PORT_CTRL2_REG);

	if (en)
		val |= MV_GMAC_PORT_CTRL2_PCS_EN_MASK;
	else
		val &= ~MV_GMAC_PORT_CTRL2_PCS_EN_MASK;

	/* enable / disable PCS on this port */
	mv_gop110_gmac_write(gop, pcs_num, MV_GMAC_PORT_CTRL2_REG, val);

	return 0;
}

static void mv_gop110_gmac_sgmii2_5_cfg(struct gop_hw *gop, int mac_num)
{
	u32 val, thresh, an;

	/*configure minimal level of the Tx FIFO before the lower part starts to read a packet*/
	thresh = MV_SGMII2_5_TX_FIFO_MIN_TH;
	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_FIFO_CFG_1_REG);
	U32_SET_FIELD(val, MV_GMAC_PORT_FIFO_CFG_1_TX_FIFO_MIN_TH_MASK,
		(thresh << MV_GMAC_PORT_FIFO_CFG_1_TX_FIFO_MIN_TH_OFFS));
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_FIFO_CFG_1_REG, val);

	/* Disable bypass of sync module */
	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL4_REG);
	val |= MV_GMAC_PORT_CTRL4_SYNC_BYPASS_MASK;
	/* configure DP clock select according to mode */
	val |= MV_GMAC_PORT_CTRL4_DP_CLK_SEL_MASK;
	/* configure QSGMII bypass according to mode */
	val |= MV_GMAC_PORT_CTRL4_QSGMII_BYPASS_ACTIVE_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL4_REG, val);

	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL2_REG);
	val |= MV_GMAC_PORT_CTRL2_DIS_PADING_OFFS;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL2_REG, val);

	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL0_REG);
	/* configure GIG MAC to 1000Base-X mode connected to a fiber transceiver */
	val |= MV_GMAC_PORT_CTRL0_PORTTYPE_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL0_REG, val);

	/* configure AN 0x9268 */
	an = MV_GMAC_PORT_AUTO_NEG_CFG_AN_BYPASS_EN_MASK |
		MV_GMAC_PORT_AUTO_NEG_CFG_SET_MII_SPEED_MASK  |
		MV_GMAC_PORT_AUTO_NEG_CFG_SET_GMII_SPEED_MASK     |
		MV_GMAC_PORT_AUTO_NEG_CFG_ADV_PAUSE_MASK    |
		MV_GMAC_PORT_AUTO_NEG_CFG_SET_FULL_DX_MASK  |
		MV_GMAC_PORT_AUTO_NEG_CFG_CHOOSE_SAMPLE_TX_CONFIG_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_AUTO_NEG_CFG_REG, an);
}

static void mv_gop110_gmac_sgmii_cfg(struct gop_hw *gop, int mac_num)
{
	u32 val, thresh, an;

	/*configure minimal level of the Tx FIFO before the lower part starts to read a packet*/
	thresh = MV_SGMII_TX_FIFO_MIN_TH;
	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_FIFO_CFG_1_REG);
	U32_SET_FIELD(val, MV_GMAC_PORT_FIFO_CFG_1_TX_FIFO_MIN_TH_MASK,
		(thresh << MV_GMAC_PORT_FIFO_CFG_1_TX_FIFO_MIN_TH_OFFS));
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_FIFO_CFG_1_REG, val);

	/* Disable bypass of sync module */
	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL4_REG);
	val |= MV_GMAC_PORT_CTRL4_SYNC_BYPASS_MASK;
	/* configure DP clock select according to mode */
	val &= ~MV_GMAC_PORT_CTRL4_DP_CLK_SEL_MASK;
	/* configure QSGMII bypass according to mode */
	val |= MV_GMAC_PORT_CTRL4_QSGMII_BYPASS_ACTIVE_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL4_REG, val);

	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL2_REG);
	val |= MV_GMAC_PORT_CTRL2_DIS_PADING_OFFS;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL2_REG, val);

	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL0_REG);
	/* configure GIG MAC to SGMII mode */
	val &= ~MV_GMAC_PORT_CTRL0_PORTTYPE_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL0_REG, val);

	/* configure AN */
	an = MV_GMAC_PORT_AUTO_NEG_CFG_EN_PCS_AN_MASK |
		MV_GMAC_PORT_AUTO_NEG_CFG_AN_BYPASS_EN_MASK |
		MV_GMAC_PORT_AUTO_NEG_CFG_EN_AN_SPEED_MASK  |
		MV_GMAC_PORT_AUTO_NEG_CFG_EN_FC_AN_MASK     |
		MV_GMAC_PORT_AUTO_NEG_CFG_EN_FDX_AN_MASK    |
		MV_GMAC_PORT_AUTO_NEG_CFG_CHOOSE_SAMPLE_TX_CONFIG_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_AUTO_NEG_CFG_REG, an);
}

static void mv_gop110_gmac_rgmii_cfg(struct gop_hw *gop, int mac_num)
{
	u32 val, thresh, an;

	/*configure minimal level of the Tx FIFO before the lower part starts to read a packet*/
	thresh = MV_RGMII_TX_FIFO_MIN_TH;
	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_FIFO_CFG_1_REG);
	U32_SET_FIELD(val, MV_GMAC_PORT_FIFO_CFG_1_TX_FIFO_MIN_TH_MASK,
		(thresh << MV_GMAC_PORT_FIFO_CFG_1_TX_FIFO_MIN_TH_OFFS));
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_FIFO_CFG_1_REG, val);

	/* Disable bypass of sync module */
	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL4_REG);
	val |= MV_GMAC_PORT_CTRL4_SYNC_BYPASS_MASK;
	/* configure DP clock select according to mode */
	val &= ~MV_GMAC_PORT_CTRL4_DP_CLK_SEL_MASK;
	val |= MV_GMAC_PORT_CTRL4_QSGMII_BYPASS_ACTIVE_MASK;
	val |= MV_GMAC_PORT_CTRL4_EXT_PIN_GMII_SEL_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL4_REG, val);

	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL2_REG);
	val &= ~MV_GMAC_PORT_CTRL2_DIS_PADING_OFFS;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL2_REG, val);

	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL0_REG);
	/* configure GIG MAC to SGMII mode */
	val &= ~MV_GMAC_PORT_CTRL0_PORTTYPE_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL0_REG, val);

	/* configure AN 0xb8e8 */
	an = MV_GMAC_PORT_AUTO_NEG_CFG_AN_BYPASS_EN_MASK |
		MV_GMAC_PORT_AUTO_NEG_CFG_EN_AN_SPEED_MASK   |
		MV_GMAC_PORT_AUTO_NEG_CFG_EN_FC_AN_MASK      |
		MV_GMAC_PORT_AUTO_NEG_CFG_EN_FDX_AN_MASK     |
		MV_GMAC_PORT_AUTO_NEG_CFG_CHOOSE_SAMPLE_TX_CONFIG_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_AUTO_NEG_CFG_REG, an);
}

static void mv_gop110_gmac_qsgmii_cfg(struct gop_hw *gop, int mac_num)
{
	u32 val, thresh, an;

	/*configure minimal level of the Tx FIFO before the lower part starts to read a packet*/
	thresh = MV_SGMII_TX_FIFO_MIN_TH;
	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_FIFO_CFG_1_REG);
	U32_SET_FIELD(val, MV_GMAC_PORT_FIFO_CFG_1_TX_FIFO_MIN_TH_MASK,
		(thresh << MV_GMAC_PORT_FIFO_CFG_1_TX_FIFO_MIN_TH_OFFS));
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_FIFO_CFG_1_REG, val);

	/* Disable bypass of sync module */
	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL4_REG);
	val |= MV_GMAC_PORT_CTRL4_SYNC_BYPASS_MASK;
	/* configure DP clock select according to mode */
	val &= ~MV_GMAC_PORT_CTRL4_DP_CLK_SEL_MASK;
	val &= ~MV_GMAC_PORT_CTRL4_EXT_PIN_GMII_SEL_MASK;
	/* configure QSGMII bypass according to mode */
	val &= ~MV_GMAC_PORT_CTRL4_QSGMII_BYPASS_ACTIVE_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL4_REG, val);

	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL2_REG);
	val &= ~MV_GMAC_PORT_CTRL2_DIS_PADING_OFFS;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL2_REG, val);

	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL0_REG);
	/* configure GIG MAC to SGMII mode */
	val &= ~MV_GMAC_PORT_CTRL0_PORTTYPE_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL0_REG, val);

	/* configure AN 0xB8EC */
	an = MV_GMAC_PORT_AUTO_NEG_CFG_EN_PCS_AN_MASK |
		MV_GMAC_PORT_AUTO_NEG_CFG_AN_BYPASS_EN_MASK |
		MV_GMAC_PORT_AUTO_NEG_CFG_EN_AN_SPEED_MASK  |
		MV_GMAC_PORT_AUTO_NEG_CFG_EN_FC_AN_MASK     |
		MV_GMAC_PORT_AUTO_NEG_CFG_EN_FDX_AN_MASK    |
		MV_GMAC_PORT_AUTO_NEG_CFG_CHOOSE_SAMPLE_TX_CONFIG_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_AUTO_NEG_CFG_REG, an);
}

void mv_gop110_gmac_port_link_event_mask(struct gop_hw *gop, int mac_num)
{
	u32 reg_val;

	reg_val = mv_gop110_gmac_read(gop, mac_num,
				MV_GMAC_INTERRUPT_SUM_MASK_REG);
	reg_val &= ~MV_GMAC_INTERRUPT_SUM_CAUSE_LINK_CHANGE_MASK;
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_INTERRUPT_SUM_MASK_REG,
			reg_val);
}

/* Set the internal mux's to the required MAC in the GOP */
int mv_gop110_gmac_mode_cfg(struct gop_hw *gop, struct mv_mac_data *mac)
{
	u32 reg_addr;
	u32 val;

	int mac_num = mac->gop_index;

	/* Set TX FIFO thresholds */
	switch (mac->phy_mode) {
	case PHY_INTERFACE_MODE_SGMII:
		if (mac->speed == 2500)
			mv_gop110_gmac_sgmii2_5_cfg(gop, mac_num);
		else
			mv_gop110_gmac_sgmii_cfg(gop, mac_num);
	break;
	case PHY_INTERFACE_MODE_RGMII:
		mv_gop110_gmac_rgmii_cfg(gop, mac_num);
	break;
	case PHY_INTERFACE_MODE_QSGMII:
		mv_gop110_gmac_qsgmii_cfg(gop, mac_num);
	break;
	default:
		return -1;
	}

	/* Jumbo frame support - 0x1400*2= 0x2800 bytes */
	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL0_REG);
	U32_SET_FIELD(val, MV_GMAC_PORT_CTRL0_FRAMESIZELIMIT_MASK,
		(0x1400 << MV_GMAC_PORT_CTRL0_FRAMESIZELIMIT_OFFS));
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL0_REG, val);

	/* PeriodicXonEn disable */
	reg_addr = MV_GMAC_PORT_CTRL1_REG;
	val = mv_gop110_gmac_read(gop, mac_num, reg_addr);
	val &= ~MV_GMAC_PORT_CTRL1_EN_PERIODIC_FC_XON_MASK;
	mv_gop110_gmac_write(gop, mac_num, reg_addr, val);

	/* mask all ports interrupts */
	mv_gop110_gmac_port_link_event_mask(gop, mac_num);

#if MV_PP2x_INTERRUPT
	/* unmask link change interrupt */
	val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_INTERRUPT_MASK_REG);
	val |= MV_GMAC_INTERRUPT_CAUSE_LINK_CHANGE_MASK;
	val |= 1; /* unmask summary bit */
	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_INTERRUPT_MASK_REG, val);
#endif
	return 0;
}

void mv_gop110_xlg_2_gig_mac_cfg(struct gop_hw *gop, int mac_num)
{
	u32 reg_val;

	/* relevant only for MAC0 (XLG0 and GMAC0) */
	if (mac_num > 0)
		return;

	/* configure 1Gig MAC mode */
	reg_val = mv_gop110_xlg_mac_read(gop, mac_num,
					MV_XLG_PORT_MAC_CTRL3_REG);
	U32_SET_FIELD(reg_val, MV_XLG_MAC_CTRL3_MACMODESELECT_MASK,
		(0 << MV_XLG_MAC_CTRL3_MACMODESELECT_OFFS));
	mv_gop110_xlg_mac_write(gop, mac_num, MV_XLG_PORT_MAC_CTRL3_REG,
				reg_val);
}

int  mv_gop110_gpcs_reset(struct gop_hw *gop, int pcs_num, enum mv_reset act)
{
	u32 reg_data;

	reg_data = mv_gop110_gmac_read(gop, pcs_num, MV_GMAC_PORT_CTRL2_REG);
	if (act == RESET)
		U32_SET_FIELD(reg_data, MV_GMAC_PORT_CTRL2_SGMII_MODE_MASK, 0);
	else
		U32_SET_FIELD(reg_data, MV_GMAC_PORT_CTRL2_SGMII_MODE_MASK,
			1 << MV_GMAC_PORT_CTRL2_SGMII_MODE_OFFS);

	mv_gop110_gmac_write(gop, pcs_num, MV_GMAC_PORT_CTRL2_REG, reg_data);
	return 0;
}

/* Set the internal mux's to the required PCS in the PI */
int mv_gop110_xpcs_mode(struct gop_hw *gop, int num_of_lanes)
{
	u32 reg_addr;
	u32 val;
	int lane;

	switch (num_of_lanes) {
	case 1:
		lane = 0;
	break;
	case 2:
		lane = 1;
	break;
	case 4:
		lane = 2;
	break;
	default:
		return -1;
	}

	/* configure XG MAC mode */
	reg_addr = MV_XPCS_GLOBAL_CFG_0_REG;
	val = mv_gop110_xpcs_global_read(gop, reg_addr);
	val &= ~MV_XPCS_GLOBAL_CFG_0_PCSMODE_MASK;
	U32_SET_FIELD(val, MV_XPCS_GLOBAL_CFG_0_PCSMODE_MASK, 0);
	U32_SET_FIELD(val, MV_XPCS_GLOBAL_CFG_0_LANEACTIVE_MASK,
		(2 * lane) << MV_XPCS_GLOBAL_CFG_0_LANEACTIVE_OFFS);
	mv_gop110_xpcs_global_write(gop, reg_addr, val);

	return 0;
}

void mv_gop110_xlg_port_link_event_mask(struct gop_hw *gop, int mac_num)
{
	u32 reg_val;

	reg_val = mv_gop110_xlg_mac_read(gop, mac_num,
					MV_XLG_EXTERNAL_INTERRUPT_MASK_REG);
	reg_val &= ~(1 << 1);
	mv_gop110_xlg_mac_write(gop, mac_num,
				MV_XLG_EXTERNAL_INTERRUPT_MASK_REG, reg_val);
}

int mv_gop110_port_events_mask(struct gop_hw *gop, struct mv_mac_data *mac)
{
	int port_num = mac->gop_index;

	switch (mac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
		mv_gop110_gmac_port_link_event_mask(gop, port_num);
	break;

	case PHY_INTERFACE_MODE_XAUI:
	case PHY_INTERFACE_MODE_RXAUI:
		mv_gop110_xlg_port_link_event_mask(gop, port_num);
	break;

	default:
		netdev_err(NULL, "%s: Wrong port mode (%d)", __func__,
				mac->phy_mode);
		return -1;
	}
	return 0;
}

/* Set the internal mux's to the required MAC in the GOP */
int mv_gop110_xlg_mac_mode_cfg(struct gop_hw *gop, int mac_num,
				int num_of_act_lanes)
{
	u32 reg_addr;
	u32 val;

	/* Set TX FIFO thresholds */
	reg_addr = MV_XLG_PORT_FIFOS_THRS_CFG_REG;
	val = mv_gop110_xlg_mac_read(gop, mac_num, reg_addr);
	U32_SET_FIELD(val, MV_XLG_MAC_PORT_FIFOS_THRS_CFG_TXRDTHR_MASK,
		(6 << MV_XLG_MAC_PORT_FIFOS_THRS_CFG_TXRDTHR_OFFS));
	mv_gop110_xlg_mac_write(gop, mac_num, reg_addr, val);

	/* configure 10G MAC mode */
	reg_addr = MV_XLG_PORT_MAC_CTRL3_REG;
	val = mv_gop110_xlg_mac_read(gop, mac_num, reg_addr);
	U32_SET_FIELD(val, MV_XLG_MAC_CTRL3_MACMODESELECT_MASK,
		(1 << MV_XLG_MAC_CTRL3_MACMODESELECT_OFFS));
	mv_gop110_xlg_mac_write(gop, mac_num, reg_addr, val);

	reg_addr = MV_XLG_PORT_MAC_CTRL4_REG;

	/* read - modify - write */
	val = mv_gop110_xlg_mac_read(gop, mac_num, reg_addr);
	U32_SET_FIELD(val, 0x1F10, 0x310);
	mv_gop110_xlg_mac_write(gop, mac_num, reg_addr, val);

	/* Jumbo frame support - 0x1400*2= 0x2800 bytes */
	val = mv_gop110_xlg_mac_read(gop, mac_num, MV_XLG_PORT_MAC_CTRL1_REG);
	U32_SET_FIELD(val, 0x1FFF, 0x1400);
	mv_gop110_xlg_mac_write(gop, mac_num, MV_XLG_PORT_MAC_CTRL1_REG, val);

	/* mask all port interrupts */
	mv_gop110_xlg_port_link_event_mask(gop, mac_num);

	/* unmask link change interrupt */

	return 0;
}

/* Set PCS to reset or exit from reset */
int mv_gop110_xpcs_reset(struct gop_hw *gop, enum mv_reset reset)
{
	u32 reg_addr;
	u32 val;

	reg_addr = MV_XPCS_GLOBAL_CFG_0_REG;

	/* read - modify - write */
	val = mv_gop110_xpcs_global_read(gop, reg_addr);
	if (reset == RESET)
		val &= ~MV_XPCS_GLOBAL_CFG_0_PCSRESET_MASK;
	else
		val |= MV_XPCS_GLOBAL_CFG_0_PCSRESET_MASK;
	mv_gop110_xpcs_global_write(gop, reg_addr, val);

	return 0;
}

/* Set the MAC to reset or exit from reset */
int mv_gop110_xlg_mac_reset(struct gop_hw *gop, int mac_num, enum mv_reset reset)
{
	u32 reg_addr;
	u32 val;

	reg_addr = MV_XLG_PORT_MAC_CTRL0_REG;

	/* read - modify - write */
	val = mv_gop110_xlg_mac_read(gop, mac_num, reg_addr);
	if (reset == RESET)
		val &= ~MV_XLG_MAC_CTRL0_MACRESETN_MASK;
	else
		val |= MV_XLG_MAC_CTRL0_MACRESETN_MASK;
	mv_gop110_xlg_mac_write(gop, mac_num, reg_addr, val);

	return 0;
}

void mv_gop110_serdes_reset(struct gop_hw *gop, int lane, bool analog_reset,
				bool core_reset, bool digital_reset)
{
	u32 reg_val;

	reg_val = mv_gop110_serdes_read(gop, lane, MV_SERDES_CFG_1_REG);
	if (analog_reset)
		reg_val &= ~MV_SERDES_CFG_1_ANALOG_RESET_MASK;
	else
		reg_val |= MV_SERDES_CFG_1_ANALOG_RESET_MASK;

	if (core_reset)
		reg_val &= ~MV_SERDES_CFG_1_CORE_RESET_MASK;
	else
		reg_val |= MV_SERDES_CFG_1_CORE_RESET_MASK;

	if (digital_reset)
		reg_val &= ~MV_SERDES_CFG_1_DIGITAL_RESET_MASK;
	else
		reg_val |= MV_SERDES_CFG_1_DIGITAL_RESET_MASK;

	mv_gop110_serdes_write(gop, lane, MV_SERDES_CFG_1_REG, reg_val);
}

void mv_gop110_serdes_init(struct gop_hw *gop, int lane,
				enum sd_media_mode mode)
{
	u32 reg_val;

	/* Media Interface Mode */
	reg_val = mv_gop110_serdes_read(gop, lane, MV_SERDES_CFG_0_REG);
	if (mode == MV_RXAUI)
		reg_val |= MV_SERDES_CFG_0_MEDIA_MODE_MASK;
	else
		reg_val &= ~MV_SERDES_CFG_0_MEDIA_MODE_MASK;

	/* Pull-Up PLL to StandAlone mode */
	reg_val |= MV_SERDES_CFG_0_PU_PLL_MASK;
	/* powers up the SD Rx/Tx PLL */
	reg_val |= MV_SERDES_CFG_0_RX_PLL_MASK;
	reg_val |= MV_SERDES_CFG_0_TX_PLL_MASK;
	mv_gop110_serdes_write(gop, lane, MV_SERDES_CFG_0_REG, reg_val);

	mv_gop110_serdes_reset(gop, lane, false, false, false);

	reg_val = 0x17f;
	mv_gop110_serdes_write(gop, lane, MV_SERDES_MISC_REG, reg_val);
}

/*
* mv_port_init
*       Init physical port. Configures the port mode and all it's elements
*       accordingly.
*       Does not verify that the selected mode/port number is valid at the
*       core level.
*/
int mv_gop110_port_init(struct gop_hw *gop, struct mv_mac_data *mac)
{
	int mac_num = mac->gop_index;
	int num_of_act_lanes;

	if (mac_num >= MVCPN110_GOP_MAC_NUM) {
		netdev_err(NULL, "%s: illegal port number %d", __func__,
				mac_num);
		return -1;
	}

	switch (mac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
		mv_gop110_gmac_reset(gop, mac_num, RESET);
		/* configure PCS */
		mv_gop110_gpcs_mode_cfg(gop, mac_num, false);

		/* configure MAC */
		mv_gop110_gmac_mode_cfg(gop, mac);
		/* pcs unreset */
		mv_gop110_gpcs_reset(gop, mac_num, UNRESET);
		/* mac unreset */
		mv_gop110_gmac_reset(gop, mac_num, UNRESET);
	break;
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
		/* configure PCS */
		mv_gop110_gpcs_mode_cfg(gop, mac_num, true);

		/* configure MAC */
		mv_gop110_gmac_mode_cfg(gop, mac);
		/* select proper Mac mode */
		mv_gop110_xlg_2_gig_mac_cfg(gop, mac_num);

		/* pcs unreset */
		mv_gop110_gpcs_reset(gop, mac_num, UNRESET);
		/* mac unreset */
		mv_gop110_gmac_reset(gop, mac_num, UNRESET);
	break;

	case PHY_INTERFACE_MODE_XAUI:
		num_of_act_lanes = 4;
		mac_num = 0;
		/* configure PCS */
		mv_gop110_xpcs_mode(gop, num_of_act_lanes);
		/* configure MAC */
		mv_gop110_xlg_mac_mode_cfg(gop, mac_num, num_of_act_lanes);

		/* pcs unreset */
		mv_gop110_xpcs_reset(gop, UNRESET);
		/* mac unreset */
		mv_gop110_xlg_mac_reset(gop, mac_num, UNRESET);
	break;
	case PHY_INTERFACE_MODE_RXAUI:
		num_of_act_lanes = 2;
		mv_gop110_serdes_init(gop, 0, MV_RXAUI); /*mapped to serdes 6*/
		mv_gop110_serdes_init(gop, 1, MV_RXAUI); /*mapped to serdes 5*/

		mac_num = 0;
		/* configure PCS */
		mv_gop110_xpcs_mode(gop, num_of_act_lanes);
		/* configure MAC */
		mv_gop110_xlg_mac_mode_cfg(gop, mac_num, num_of_act_lanes);

		/* pcs unreset */
		mv_gop110_xpcs_reset(gop, UNRESET);

		/* mac unreset */;
		mv_gop110_xlg_mac_reset(gop, mac_num, UNRESET);

		/* run digital reset / unreset */
		mv_gop110_serdes_reset(gop, 0, false, false, true);
		mv_gop110_serdes_reset(gop, 1, false, false, true);
		mv_gop110_serdes_reset(gop, 0, false, false, false);
		mv_gop110_serdes_reset(gop, 1, false, false, false);
	break;

	default:
		netdev_err(NULL, "%s: Requested port mode (%d) not supported",
				__func__, mac->phy_mode);
		return -1;
	}

	return 0;
}

/* Sets port speed to Auto Negotiation / 1000 / 100 / 10 Mbps.
*  Sets port duplex to Auto Negotiation / Full / Half Duplex.
*/
int mv_gop110_gmac_speed_duplex_set(struct gop_hw *gop,
	int mac_num, enum mv_port_speed speed, enum mv_port_duplex duplex)
{
	u32 reg_val;

	/* Check validity */
	if ((speed == MV_PORT_SPEED_1000) && (duplex == MV_PORT_DUPLEX_HALF))
		return -EINVAL;

	reg_val = mv_gop110_gmac_read(gop, mac_num,
					MV_GMAC_PORT_AUTO_NEG_CFG_REG);

	switch (speed) {
	case MV_PORT_SPEED_AN:
		reg_val |= MV_GMAC_PORT_AUTO_NEG_CFG_EN_AN_SPEED_MASK;
		/* the other bits don't matter in this case */
		break;
	case MV_PORT_SPEED_1000:
		reg_val &= ~MV_GMAC_PORT_AUTO_NEG_CFG_EN_AN_SPEED_MASK;
		reg_val |= MV_GMAC_PORT_AUTO_NEG_CFG_SET_GMII_SPEED_MASK;
		/* the 100/10 bit doesn't matter in this case */
		break;
	case MV_PORT_SPEED_100:
		reg_val &= ~MV_GMAC_PORT_AUTO_NEG_CFG_EN_AN_SPEED_MASK;
		reg_val &= ~MV_GMAC_PORT_AUTO_NEG_CFG_SET_GMII_SPEED_MASK;
		reg_val |= MV_GMAC_PORT_AUTO_NEG_CFG_SET_MII_SPEED_MASK;
		break;
	case MV_PORT_SPEED_10:
		reg_val &= ~MV_GMAC_PORT_AUTO_NEG_CFG_EN_AN_SPEED_MASK;
		reg_val &= ~MV_GMAC_PORT_AUTO_NEG_CFG_SET_GMII_SPEED_MASK;
		reg_val &= ~MV_GMAC_PORT_AUTO_NEG_CFG_SET_MII_SPEED_MASK;
		break;
	default:
		netdev_info(NULL, "GMAC: Unexpected Speed value %d\n", speed);
		return -EINVAL;
	}

	switch (duplex) {
	case MV_PORT_DUPLEX_AN:
		reg_val  |= MV_GMAC_PORT_AUTO_NEG_CFG_EN_FDX_AN_MASK;
		/* the other bits don't matter in this case */
		break;
	case MV_PORT_DUPLEX_HALF:
		reg_val &= ~MV_GMAC_PORT_AUTO_NEG_CFG_EN_FDX_AN_MASK;
		reg_val &= ~MV_GMAC_PORT_AUTO_NEG_CFG_SET_FULL_DX_MASK;
		break;
	case MV_PORT_DUPLEX_FULL:
		reg_val &= ~MV_GMAC_PORT_AUTO_NEG_CFG_EN_FDX_AN_MASK;
		reg_val |= MV_GMAC_PORT_AUTO_NEG_CFG_SET_FULL_DX_MASK;
		break;
	default:
		netdev_err(NULL, "GMAC: Unexpected Duplex value %d\n", duplex);
		return -EINVAL;
	}

	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_AUTO_NEG_CFG_REG,
				reg_val);
	return 0;
}

/* Sets port speed to Auto Negotiation / 1000 / 100 / 10 Mbps.
*  Sets port duplex to Auto Negotiation / Full / Half Duplex.
*/
int mv_gop110_xlg_mac_speed_duplex_set(struct gop_hw *gop, int mac_num,
			enum mv_port_speed speed, enum mv_port_duplex duplex)
{
	/* not supported */
	return -1;
}

/* set port speed and duplex */
int mv_gop110_speed_duplex_set(struct gop_hw *gop, struct mv_mac_data *mac,
			enum mv_port_speed speed, enum mv_port_duplex duplex)
{
	int port_num = mac->gop_index;

	switch (mac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
		mv_gop110_gmac_speed_duplex_set(gop, port_num, speed, duplex);
	break;

	case PHY_INTERFACE_MODE_XAUI:
	case PHY_INTERFACE_MODE_RXAUI:
		mv_gop110_xlg_mac_speed_duplex_set(gop, port_num,
			speed, duplex);
	break;

	default:
		netdev_err(NULL, "%s: Wrong port mode (%d)",
				__func__, mac->phy_mode);
		return -1;
	}
	return 0;
}

/* Sets "Force Link Pass" and "Do Not Force Link Fail" bits.
*  This function should only be called when the port is disabled.
*/
int mv_gop110_gmac_force_link_mode_set(struct gop_hw *gop,
			int mac_num, bool force_link_up, bool force_link_down)
{
	u32 reg_val;

	/* Can't force link pass and link fail at the same time */
	if ((force_link_up) && (force_link_down))
		return -EINVAL;

	reg_val = mv_gop110_gmac_read(gop, mac_num,
					MV_GMAC_PORT_AUTO_NEG_CFG_REG);

	if (force_link_up)
		reg_val |= MV_GMAC_PORT_AUTO_NEG_CFG_FORCE_LINK_UP_MASK;
	else
		reg_val &= ~MV_GMAC_PORT_AUTO_NEG_CFG_FORCE_LINK_UP_MASK;

	if (force_link_down)
		reg_val |= MV_GMAC_PORT_AUTO_NEG_CFG_FORCE_LINK_DOWN_MASK;
	else
		reg_val &= ~MV_GMAC_PORT_AUTO_NEG_CFG_FORCE_LINK_DOWN_MASK;

	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_AUTO_NEG_CFG_REG,
				reg_val);

	return 0;
}

int mv_gop110_fl_cfg(struct gop_hw *gop, struct mv_mac_data *mac)
{
	int port_num = mac->gop_index;

	switch (mac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
		/* disable AN */
		if (mac->speed == 2500)
			mv_gop110_speed_duplex_set(gop, mac, MV_PORT_SPEED_2000,
							MV_PORT_DUPLEX_FULL);
		else
			mv_gop110_speed_duplex_set(gop, mac, MV_PORT_SPEED_1000,
							MV_PORT_DUPLEX_FULL);
		/* force link */
		mv_gop110_gmac_force_link_mode_set(gop, port_num, true, false);
	break;

	case PHY_INTERFACE_MODE_XAUI:
	case PHY_INTERFACE_MODE_RXAUI:
		return 0;

	default:
		netdev_err(NULL, "%s: Wrong port mode (%d)", __func__,
				mac->phy_mode);
		return -1;
	}
	return 0;
}

/* Enable port and MIB counters */
void mv_gop110_gmac_port_enable(struct gop_hw *gop, int mac_num)
{
	u32 reg_val;

	reg_val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL0_REG);
	reg_val |= MV_GMAC_PORT_CTRL0_PORTEN_MASK;
	reg_val |= MV_GMAC_PORT_CTRL0_COUNT_EN_MASK;

	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL0_REG, reg_val);
}

/* Disable port */
void mv_gop110_gmac_port_disable(struct gop_hw *gop, int mac_num)
{
	u32 reg_val;

	/* mask all ports interrupts */
	mv_gop110_gmac_port_link_event_mask(gop, mac_num);

	reg_val = mv_gop110_gmac_read(gop, mac_num, MV_GMAC_PORT_CTRL0_REG);
	reg_val &= ~MV_GMAC_PORT_CTRL0_PORTEN_MASK;

	mv_gop110_gmac_write(gop, mac_num, MV_GMAC_PORT_CTRL0_REG, reg_val);
}

/* Enable port and MIB counters update */
void mv_gop110_xlg_mac_port_enable(struct gop_hw *gop, int mac_num)
{
	u32 reg_val;

	reg_val = mv_gop110_xlg_mac_read(gop, mac_num,
					MV_XLG_PORT_MAC_CTRL0_REG);
	reg_val |= MV_XLG_MAC_CTRL0_PORTEN_MASK;
	reg_val &= ~MV_XLG_MAC_CTRL0_MIBCNTDIS_MASK;

	mv_gop110_xlg_mac_write(gop, mac_num, MV_XLG_PORT_MAC_CTRL0_REG,
				reg_val);
}

/* Disable port */
void mv_gop110_xlg_mac_port_disable(struct gop_hw *gop, int mac_num)
{
	u32 reg_val;

	/* mask all port interrupts */
	mv_gop110_xlg_port_link_event_mask(gop, mac_num);

	reg_val = mv_gop110_xlg_mac_read(gop, mac_num,
					MV_XLG_PORT_MAC_CTRL0_REG);
	reg_val &= ~MV_XLG_MAC_CTRL0_PORTEN_MASK;

	mv_gop110_xlg_mac_write(gop, mac_num, MV_XLG_PORT_MAC_CTRL0_REG,
				reg_val);
}

void mv_gop110_port_enable(struct gop_hw *gop, struct mv_mac_data *mac)
{
	int port_num = mac->gop_index;

	switch (mac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
		mv_gop110_gmac_port_enable(gop, port_num);
	break;

	case PHY_INTERFACE_MODE_XAUI:
	case PHY_INTERFACE_MODE_RXAUI:
		mv_gop110_xlg_mac_port_enable(gop, port_num);

	break;
	default:
		netdev_err(NULL, "%s: Wrong port mode (%d)", __func__,
				mac->phy_mode);
		return;
	}
}

void mv_gop110_port_disable(struct gop_hw *gop, struct mv_mac_data *mac)
{
	int port_num = mac->gop_index;

	switch (mac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
		mv_gop110_gmac_port_disable(gop, port_num);
	break;

	case PHY_INTERFACE_MODE_XAUI:
	case PHY_INTERFACE_MODE_RXAUI:
		mv_gop110_xlg_mac_port_disable(gop, port_num);
	break;

	default:
		netdev_err(NULL, "%s: Wrong port mode (%d)", __func__,
				mac->phy_mode);
		return;
	}
}

/*
* mv_gop110_smi_init
*/
int mv_gop110_smi_init(struct gop_hw *gop)
{
	u32 val;

	/* not invert MDC */
	val = mv_gop110_smi_read(gop, MV_SMI_MISC_CFG_REG);
	val &= ~MV_SMI_MISC_CFG_INVERT_MDC_MASK;
	mv_gop110_smi_write(gop, MV_SMI_MISC_CFG_REG, val);

	return 0;
}
#endif

/* Set defaults to the MVPP2 port */
static void mv_pp2x_defaults_set(struct mv_pp2x_port *port)
{
	int tx_port_num, val, queue, ptxq;

#ifdef CONFIG_MVPP2_FPGA
	writel(0x8be4, port->base);
	writel(0xc200, port->base + 0x8);
	writel(0x3, port->base + 0x90);

	writel(0x902A, port->base + 0xC);    /*force link to 100Mb*/
	writel(0x8be5, port->base);          /*enable port        */
#endif
	/* Disable Legacy WRR, Disable EJP, Release from reset */
	tx_port_num = mv_pp2x_egress_port(port);
	mv_pp2x_write(port->pp2, MVPP2_TXP_SCHED_PORT_INDEX_REG,
		    tx_port_num);
	mv_pp2x_write(port->pp2, MVPP2_TXP_SCHED_CMD_1_REG, 0);

	/* Close bandwidth for all queues */
	for (queue = 0; queue < MVPP2_MAX_TXQ; queue++) {
		ptxq = mv_pp2x_txq_phys(port->id, queue);
		mv_pp2x_write(port->pp2,
			    MVPP2_TXQ_SCHED_TOKEN_CNTR_REG(ptxq), 0);
	}

	/* Set refill period to 1 usec, refill tokens
	 * and bucket size to maximum
	 */
	mv_pp2x_write(port->pp2, MVPP2_TXP_SCHED_PERIOD_REG,
		   MVPP2_TXP_SCHED_PERIOD_VAL);
	val = mv_pp2x_read(port->pp2, MVPP2_TXP_SCHED_REFILL_REG);
	val &= ~MVPP2_TXP_REFILL_PERIOD_ALL_MASK;
	val |= MVPP2_TXP_REFILL_PERIOD_MASK(1);
	val |= MVPP2_TXP_REFILL_TOKENS_ALL_MASK;
	mv_pp2x_write(port->pp2, MVPP2_TXP_SCHED_REFILL_REG, val);
	val = MVPP2_TXP_TOKEN_SIZE_MAX;
	mv_pp2x_write(port->pp2, MVPP2_TXP_SCHED_TOKEN_SIZE_REG, val);

	/* Set MaximumLowLatencyPacketSize value to 256 */
	mv_pp2x_write(port->pp2, MVPP2_RX_CTRL_REG(port->id),
		    MVPP2_RX_USE_PSEUDO_FOR_CSUM_MASK |
		    MVPP2_RX_LOW_LATENCY_PKT_SIZE(256));

	/* Mask all interrupts to all present cpus */
	mv_pp2x_write(port->pp2, MVPP2_ISR_ENABLE_REG(port->id),
			MVPP2_ISR_DISABLE_INTERRUPT(0x1));
}

/* Enable/disable fetching descriptors from initialized TXQs */
void mv_pp2x_egress_enable(struct mv_pp2x_port *port)
{
	u32 qmap;
	int queue;
	int tx_port_num = mv_pp2x_egress_port(port);

	/* Enable all initialized TXs. */
	qmap = 0;
	for (queue = 0; queue < txq_number; queue++) {
		struct mv_pp2x_tx_queue *txq = &port->txqs[queue];

		if (txq->descs)
			qmap |= (1 << queue);
	}

	mv_pp2x_write(port->pp2, MVPP2_TXP_SCHED_PORT_INDEX_REG, tx_port_num);
	mv_pp2x_write(port->pp2, MVPP2_TXP_SCHED_Q_CMD_REG, qmap);
}

/* Disable transmit via physical egress queue
 * - HW doesn't take descriptors from DRAM
 */
void mv_pp2x_egress_disable(struct mv_pp2x_port *port)
{
	u32 reg_data;
	int delay;
	int tx_port_num = mv_pp2x_egress_port(port);

	/* Issue stop command for active channels only */
	mv_pp2x_write(port->pp2, MVPP2_TXP_SCHED_PORT_INDEX_REG, tx_port_num);
	reg_data = (mv_pp2x_read(port->pp2, MVPP2_TXP_SCHED_Q_CMD_REG)) &
		    MVPP2_TXP_SCHED_ENQ_MASK;
	if (reg_data != 0)
		mv_pp2x_write(port->pp2, MVPP2_TXP_SCHED_Q_CMD_REG,
			    (reg_data << MVPP2_TXP_SCHED_DISQ_OFFSET));

	/* Wait for all Tx activity to terminate. */
	delay = 0;
	do {
		if (delay >= MVPP2_TX_DISABLE_TIMEOUT_MSEC)
			break;
		mdelay(1);
		delay++;

		/* Check port TX Command register that all
		 * Tx queues are stopped
		 */
		reg_data = mv_pp2x_read(port->pp2, MVPP2_TXP_SCHED_Q_CMD_REG);
	} while (reg_data & MVPP2_TXP_SCHED_ENQ_MASK);
}

/* Set IEEE 802.3x Flow Control Xon Packet Transmission Mode */
void mv_pp2x_port_periodic_xon_disable(struct mv_pp2x_port *port)
{
	u32 val;

	val = readl(port->base + MVPP2_GMAC_CTRL_1_REG) &
		    ~MVPP2_GMAC_PERIODIC_XON_EN_MASK;
	writel(val, port->base + MVPP2_GMAC_CTRL_1_REG);
}

/* Configure loopback port */
void mv_pp2x_port_loopback_set(struct mv_pp2x_port *port)
{
	u32 val;

	val = readl(port->base + MVPP2_GMAC_CTRL_1_REG);

	if (port->mac_data.speed == 1000)
		val |= MVPP2_GMAC_GMII_LB_EN_MASK;
	else
		val &= ~MVPP2_GMAC_GMII_LB_EN_MASK;

	if (port->mac_data.phy_mode == PHY_INTERFACE_MODE_SGMII)
		val |= MVPP2_GMAC_PCS_LB_EN_MASK;
	else
		val &= ~MVPP2_GMAC_PCS_LB_EN_MASK;

	writel(val, port->base + MVPP2_GMAC_CTRL_1_REG);
}

void mv_pp2x_port_reset(struct mv_pp2x_port *port)
{
	u32 val;

	val = readl(port->base + MVPP2_GMAC_CTRL_2_REG) &
		    ~MVPP2_GMAC_PORT_RESET_MASK;
	writel(val, port->base + MVPP2_GMAC_CTRL_2_REG);

	while (readl(port->base + MVPP2_GMAC_CTRL_2_REG) &
	       MVPP2_GMAC_PORT_RESET_MASK)
		continue;
}

void mv_pp2x1_port_power_up(struct mv_pp2x_port *port)
{
	mv_pp2x_port_mii_set(port);
	mv_pp2x_port_periodic_xon_disable(port);
	mv_pp2x_port_fc_adv_enable(port);
	mv_pp2x_port_reset(port);
}

/* Update HW with number of RX descriptors processed by SW:
 *    - decrement number of occupied descriptors
 *    - increment number of Non-occupied descriptors
 */
static void mv_pp2x_rxq_desc_num_update(struct mv_pp2x_port *port,
					int rxq, int rx_done, int rx_filled)
{
	u32 reg_val;
	int prxq = port->rxqs[rxq].id;

	reg_val = (rx_done << MVPP2_RXQ_NUM_PROCESSED_OFFSET) | (rx_filled
			<< MVPP2_RXQ_NUM_NEW_OFFSET);
	mv_pp2x_write(port->pp2, MVPP2_RXQ_STATUS_UPDATE_REG(prxq), reg_val);
}

/* Get pointer to next Rx descriptor to be processed (send) by HW */
static struct mv_pp2x_rx_desc *mv_pp2x_rxq_next_desc_get(struct mv_pp2x_rx_queue *rxq)
{
	int rx_desc = rxq->next_desc_to_proc;

	rxq->next_desc_to_proc = MVPP2_QUEUE_NEXT_DESC(rxq, rx_desc);
	return rxq->descs + rx_desc;
}

/* Get number of Rx descriptors occupied by received packets */
static int mv_pp2x_rxq_received(struct mv_pp2x_port *pp, int rxq_id)
{
	u32 val = mv_pp2x_read(pp->pp2, MVPP2_RXQ_STATUS_REG(rxq_id));

	return val & MVPP2_RXQ_OCCUPIED_MASK;
}

/* Update Rx queue status with the number of occupied and available
 * Rx descriptor slots.
 */
static void mv_pp2x_rxq_status_update(struct mv_pp2x_port *pp, int rxq_id,
			int used_count, int free_count)
{
	/* Decrement the number of used descriptors and increment count
	 * increment the number of free descriptors.
	 */
	u32 val = used_count | (free_count << MVPP2_RXQ_NUM_NEW_OFFSET);

	mv_pp2x_write(pp->pp2, MVPP2_RXQ_STATUS_UPDATE_REG(rxq_id), val);
}

/* Tx descriptors helper methods */

/* Get number of Tx descriptors waiting to be transmitted by HW */
static int mv_pp2x_txq_pend_desc_num_get(struct mv_pp2x_port *pp,
				       struct mv_pp2x_tx_queue *txq)
{
	u32 reg_val;

	mv_pp2x_write(pp->pp2, MVPP2_TXQ_NUM_REG, txq->id);
	reg_val = mv_pp2x_read(pp->pp2, MVPP2_TXQ_PENDING_REG);

	return reg_val & MVPP2_TXQ_PENDING_MASK;
}

/* Get pointer to next Tx descriptor to be processed (send) by HW */
static struct mv_pp2x_tx_desc *mv_pp2x_txq_next_desc_get(struct mv_pp2x_tx_queue *txq)
{
	int tx_desc = txq->next_desc_to_proc;

	txq->next_desc_to_proc = MVPP2_QUEUE_NEXT_DESC(txq, tx_desc);

	return txq->descs + tx_desc;
}

/* Update HW with number of aggregated Tx descriptors to be sent */
static void mv_pp2x_aggr_txq_pend_desc_add(struct mv_pp2x_port *pp, int pending)
{
	/* aggregated access - relevant TXQ number is written in TX desc */
	mv_pp2x_write(pp->pp2, MVPP2_AGGR_TXQ_UPDATE_REG, pending);
}

/* Get number of occupied aggregated Tx descriptors */
static u32 mv_pp2x_aggr_txq_pend_desc_num_get(struct mv_pp2x *pp2, int cpu)
{
	u32 reg_val;

	reg_val = mv_pp2x_read(pp2, MVPP2_AGGR_TXQ_STATUS_REG(cpu));

	return reg_val & MVPP2_AGGR_TXQ_PENDING_MASK;
}

/* Set the number of packets that will be received before Rx interrupt
 * will be generated by HW.
 */
void mv_pp2x_rx_pkts_coal_set(struct mv_pp2x_port *pp,
				   struct mv_pp2x_rx_queue *rxq, u32 pkts)
{
	u32 val;

	val = (pkts & MVPP2_OCCUPIED_THRESH_MASK);
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_NUM_REG, rxq->id);
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_THRESH_REG, val);

	rxq->pkts_coal = pkts;
}

int mv_pp2x_rxq_bm_long_pool_set(struct mv_pp2x_port *pp, int rxq, int longPool)
{
	u32 regVal = 0;
	int prxq = pp->first_rxq + rxq;

	regVal = mv_pp2x_read(pp->pp2, MVPP2_RXQ_CONFIG_REG(prxq));
	regVal &= ~MVPP2_RXQ_POOL_LONG_MASK;
	regVal |= ((longPool << MVPP2_RXQ_POOL_LONG_OFFS)
			& MVPP2_RXQ_POOL_LONG_MASK);

	mv_pp2x_write(pp->pp2, MVPP2_RXQ_CONFIG_REG(prxq), regVal);

	return 0;
}

int mv_pp2x_rxq_bm_short_pool_set(struct mv_pp2x_port *pp, int rxq, int shortPool)
{
	u32 regVal = 0;
	int prxq = pp->first_rxq + rxq;

	regVal = mv_pp2x_read(pp->pp2, MVPP2_RXQ_CONFIG_REG(prxq));
	regVal &= ~MVPP2_RXQ_POOL_SHORT_MASK;
	regVal |= ((shortPool << MVPP2_RXQ_POOL_SHORT_OFFS)
			& MVPP2_RXQ_POOL_SHORT_MASK);

	mv_pp2x_write(pp->pp2, MVPP2_RXQ_CONFIG_REG(prxq), regVal);

	return 0;
}

/* Create a specified Rx queue */
static int mv_pp2x_rxq_init(struct mv_pp2x_port *pp,
			   struct mv_pp2x_rx_queue *rxq)

{
	rxq->size = pp->rx_ring_size;

	/* Allocate memory for RX descriptors */
	rxq->descs_phys = (dma_addr_t)rxq->descs;
	if (!rxq->descs)
		return -ENOMEM;

	rxq->last_desc = rxq->size - 1;

	/* Zero occupied and non-occupied counters - direct access */
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_STATUS_REG(rxq->id), 0);

	/* Set Rx descriptors queue starting address - indirect access */
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_NUM_REG, rxq->id);
#ifdef CONFIG_MVPPV22
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_DESC_ADDR_REG,
			rxq->descs_phys >> MVPP22_DESC_ADDR_SHIFT);
#else
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_DESC_ADDR_REG,
			rxq->descs_phys >> MVPP21_DESC_ADDR_SHIFT);
#endif
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_DESC_SIZE_REG, rxq->size);
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_INDEX_REG, 0);

	mv_pp2x_rx_pkts_coal_set(pp, rxq, MVPP2_RX_COAL_PKTS);

	/* Add number of descriptors ready for receiving packets */
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_STATUS_UPDATE_REG(rxq->id),
			(rxq->size << MVPP2_RXQ_NUM_NEW_OFFSET));

	return 0;
}

/* Push packets received by the RXQ to BM pool */
static void mv_pp2x_rxq_drop_pkts(struct mv_pp2x_port *pp,
				struct mv_pp2x_rx_queue *rxq)
{
	int rx_received;

	rx_received = mv_pp2x_rxq_received(pp, rxq->id);
	if (!rx_received)
		return;
	mv_pp2x_rxq_status_update(pp, rxq->id, rx_received, rx_received);
}

/* Cleanup Rx queue */
static void mv_pp2x_rxq_deinit(struct mv_pp2x_port *pp,
			     struct mv_pp2x_rx_queue *rxq)
{
	mv_pp2x_rxq_drop_pkts(pp, rxq);

	rxq->descs             = NULL;
	rxq->last_desc         = 0;
	rxq->next_desc_to_proc = 0;
	rxq->descs_phys        = 0;

	/* Clear Rx descriptors queue starting address and size;
	 * free descriptor number
	 */
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_STATUS_REG(rxq->id), 0);
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_NUM_REG, rxq->id);
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_DESC_ADDR_REG, 0);
	mv_pp2x_write(pp->pp2, MVPP2_RXQ_DESC_SIZE_REG, 0);
}

/* Create and initialize a Tx queue */
static int mv_pp2x_txq_init(struct mv_pp2x_port *pp, int txp,
			  struct mv_pp2x_tx_queue *txq)
{
	u32 reg_val;
	int desc;
	int desc_per_txq;
	int tx_port_num;

	/* Allocate memory for Tx descriptors */
	txq->descs_phys = (dma_addr_t)txq->descs;
	if (!txq->descs)
		return -ENOMEM;

	txq->last_desc = txq->size - 1;

	/* Set Tx descriptors queue starting address - indirect access */
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_NUM_REG, txq->id);
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_DESC_ADDR_LOW_REG, txq->descs_phys
			>> MVPP2_TXQ_DESC_ADDR_LOW_SHIFT);
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_DESC_SIZE_REG, txq->size
			& MVPP2_TXQ_DESC_SIZE_MASK);
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_INDEX_REG, 0);
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_RSVD_CLR_REG, txq->id <<
			MVPP2_TXQ_RSVD_CLR_OFFSET);
	reg_val = mv_pp2x_read(pp->pp2, MVPP2_TXQ_PENDING_REG);
	reg_val &= ~MVPP2_TXQ_PENDING_MASK;
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_PENDING_REG, reg_val);

	/* Calculate base address in prefetch buffer. We reserve 16 descriptors
	 * for each existing TXQ.
	 * TCONTS for PON port must be continuous from 0 to MVPP2_MAX_TCONT
	 * GBE ports assumed to be continious from 0 to MVPP2_MAX_PORTS
	 */
	desc_per_txq = 16;
	desc = (pp->id * MVPP2_MAX_TXQ * desc_per_txq) +
	       (txq->log_id * desc_per_txq);

	mv_pp2x_write(pp->pp2, MVPP2_TXQ_PREF_BUF_REG,
		    MVPP2_PREF_BUF_PTR(desc) | MVPP2_PREF_BUF_SIZE_16 |
		    MVPP2_PREF_BUF_THRESH(desc_per_txq / 2));

	/* WRR / EJP configuration - indirect access */
	tx_port_num = mv_pp2x_egress_port(pp);
	mv_pp2x_write(pp->pp2, MVPP2_TXP_SCHED_PORT_INDEX_REG, tx_port_num);

	reg_val = mv_pp2x_read(pp->pp2, MVPP2_TXQ_SCHED_REFILL_REG(txq->log_id));
	reg_val &= ~MVPP2_TXQ_REFILL_PERIOD_ALL_MASK;
	reg_val |= MVPP2_TXQ_REFILL_PERIOD_MASK(1);
	reg_val |= MVPP2_TXQ_REFILL_TOKENS_ALL_MASK;
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_SCHED_REFILL_REG(txq->log_id), reg_val);

	reg_val = MVPP2_TXQ_TOKEN_SIZE_MAX;
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_SCHED_TOKEN_SIZE_REG(txq->log_id),
		    reg_val);

	return 0;
}

/* Free allocated TXQ resources */
static void mv_pp2x_txq_deinit(struct mv_pp2x_port *pp,
			     struct mv_pp2x_tx_queue *txq)
{
	txq->descs             = NULL;
	txq->last_desc         = 0;
	txq->next_desc_to_proc = 0;
	txq->descs_phys        = 0;

	/* Set minimum bandwidth for disabled TXQs */
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_SCHED_TOKEN_CNTR_REG(txq->id), 0);

	/* Set Tx descriptors queue starting address and size */
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_NUM_REG, txq->id);
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_DESC_ADDR_LOW_REG, 0);
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_DESC_SIZE_REG, 0);
}

/* Cleanup Tx ports */
static void mv_pp2x_txp_clean(struct mv_pp2x_port *pp, int txp,
			    struct mv_pp2x_tx_queue *txq)
{
	int delay, pending;
	u32 reg_val;

	mv_pp2x_write(pp->pp2, MVPP2_TXQ_NUM_REG, txq->id);
	reg_val = mv_pp2x_read(pp->pp2, MVPP2_TXQ_PREF_BUF_REG);
	reg_val |= MVPP2_TXQ_DRAIN_EN_MASK;
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_PREF_BUF_REG, reg_val);

	/* The napi queue has been stopped so wait for all packets
	 * to be transmitted.
	 */
	delay = 0;
	do {
		if (delay >= MVPP2_TX_PENDING_TIMEOUT_MSEC) {
			printf("port %d: cleaning queue %d timed out\n",
				    pp->id, txq->log_id);
			break;
		}
		mdelay(1);
		delay++;

		pending = mv_pp2x_txq_pend_desc_num_get(pp, txq);
	} while (pending);

	reg_val &= ~MVPP2_TXQ_DRAIN_EN_MASK;
	mv_pp2x_write(pp->pp2, MVPP2_TXQ_PREF_BUF_REG, reg_val);
}

/* Cleanup all Tx queues */
static void mv_pp2x_cleanup_txqs(struct mv_pp2x_port *pp)
{
	struct mv_pp2x_tx_queue *txq;
	int txp, queue;
	u32 reg_val;

	reg_val = mv_pp2x_read(pp->pp2, MVPP2_TX_PORT_FLUSH_REG);

	/* Reset Tx ports and delete Tx queues */
	for (txp = 0; txp < pp->txp_num; txp++) {
		reg_val |= MVPP2_TX_PORT_FLUSH_MASK(pp->id);
		mv_pp2x_write(pp->pp2, MVPP2_TX_PORT_FLUSH_REG, reg_val);

		for (queue = 0; queue < txq_number; queue++) {
			txq = &pp->txqs[txp * txq_number + queue];
			mv_pp2x_txp_clean(pp, txp, txq);
			mv_pp2x_txq_deinit(pp, txq);
		}

		reg_val &= ~MVPP2_TX_PORT_FLUSH_MASK(pp->id);
		mv_pp2x_write(pp->pp2, MVPP2_TX_PORT_FLUSH_REG, reg_val);
	}
}

static int mv_pp2x_aggr_txq_init(struct mv_pp2x *pp2,
			struct mv_pp2x_tx_queue *aggr_txq)
{
	/* Allocate memory for Tx descriptors */
	aggr_txq->descs_phys = (dma_addr_t)aggr_txq->descs;
	if (!aggr_txq->descs)
		return -ENOMEM;

	aggr_txq->last_desc = aggr_txq->size - 1;

	/* Aggr TXQ no reset WA */
	aggr_txq->next_desc_to_proc = mv_pp2x_read(pp2,
						 MVPP2_AGGR_TXQ_INDEX_REG(0));

	/* Set Tx descriptors queue starting address */
	/* indirect access */
#ifdef CONFIG_MVPPV22
	mv_pp2x_write(pp2, MVPP2_AGGR_TXQ_DESC_ADDR_REG(0), aggr_txq->descs_phys
			>> MVPP22_DESC_ADDR_SHIFT);
#else
	mv_pp2x_write(pp2, MVPP2_AGGR_TXQ_DESC_ADDR_REG(0), aggr_txq->descs_phys
			>> MVPP21_DESC_ADDR_SHIFT);
#endif
	mv_pp2x_write(pp2, MVPP2_AGGR_TXQ_DESC_SIZE_REG(0), aggr_txq->size
			& MVPP2_AGGR_TXQ_DESC_SIZE_MASK);

	return 0;
}

/* Cleanup all Rx queues */
static void mv_pp2x_cleanup_rxqs(struct mv_pp2x_port *pp)
{
	int queue;

	for (queue = 0; queue < rxq_number; queue++)
		mv_pp2x_rxq_deinit(pp, &pp->rxqs[queue]);
}

/* Init all Rx queues for port */
static int mv_pp2x_setup_rxqs(struct mv_pp2x_port *pp)
{
	int queue, err;

	for (queue = 0; queue < rxq_number; queue++) {
		err = mv_pp2x_rxq_init(pp, &pp->rxqs[queue]);
		if (err)
			goto err_cleanup;
	}
	return 0;

err_cleanup:
	mv_pp2x_cleanup_rxqs(pp);
	return err;
}

/* Init all tx queues for port */
static int mv_pp2x_setup_txqs(struct mv_pp2x_port *pp)
{
	struct mv_pp2x_tx_queue *txq;
	int txp, queue, err;

	for (txp = 0; txp < pp->txp_num; txp++) {
		for (queue = 0; queue < txq_number; queue++) {
			txq = &pp->txqs[txp * txq_number + queue];
			err = mv_pp2x_txq_init(pp, txp, txq);
			if (err)
				goto err_cleanup;
		}
	}
	return 0;

err_cleanup:
	mv_pp2x_cleanup_txqs(pp);
	return err;
}

/* Init all aggr tx queues for cpu0 */
static int mv_pp2x_setup_aggr_txqs(struct mv_pp2x_port *pp)
{
	struct mv_pp2x_tx_queue *aggr_txq;
	int err = 0;

	aggr_txq = pp->pp2->aggr_txqs;
	err = mv_pp2x_aggr_txq_init(pp->pp2, aggr_txq);

	return err;
}

static void mv_pp2x_start_dev(struct mv_pp2x_port *pp)
{
	mv_pp2x_ingress_enable(pp, true);

	/* Config classifier decoding table */
	mv_pp2x_cls_port_default_config(pp);
	mv_pp2x_cls_oversize_rxq_set(pp);
#if defined(CONFIG_MVPP2_FPGA) || defined(CONFIG_MVPPV21)
	/* start the Rx/Tx activity */
	mv_pp2x_port_enable(pp);
#else
	mv_gop110_port_events_mask(&pp->pp2->gop, &pp->mac_data);
	mv_gop110_port_enable(&pp->pp2->gop, &pp->mac_data);
#endif
	mdelay(2);

	mv_pp2x_egress_enable(pp);
}

static int mv_pp2x_open(struct eth_device *dev)
{
	struct mv_pp2x_port *pp = dev->priv;
	int ret;
	unsigned char mac_bcast[ETH_ALEN] = { 0xff, 0xff, 0xff,
					      0xff, 0xff, 0xff };

	ret = mv_pp2x_prs_mac_da_accept(pp, mac_bcast, true);
	if (ret) {
		netdev_err(dev, "mv_pp2x_prs_mac_da_accept BC failed\n");
		return ret;
	}
	ret = mv_pp2x_prs_mac_da_accept(pp, dev->enetaddr, true);
	if (ret) {
		netdev_err(dev, "mv_pp2x_prs_mac_da_accept MC failed\n");
		return ret;
	}
	ret = mv_pp2x_prs_tag_mode_set(pp, MVPP2_TAG_TYPE_MH);
	if (ret) {
		netdev_err(dev, "mv_pp2x_prs_tag_mode_set failed\n");
		return ret;
	}
	ret = mv_pp2x_prs_def_flow(pp);
	if (ret) {
		netdev_err(dev, "mv_pp2x_prs_def_flow failed\n");
		return ret;
	}

	ret = mv_pp2x_setup_rxqs(pp);
	if (ret)
		return ret;

	ret = mv_pp2x_setup_txqs(pp);
	if (ret)
		return ret;

	ret = mv_pp2x_setup_aggr_txqs(pp);
	if (ret)
		return ret;

	mv_pp2x_start_dev(pp);

	return 0;
}

static int mv_pp2x_port_init(struct mv_pp2x_port *pp)
{
	int queue;

	/* Disable port */
	mv_pp2x_egress_disable(pp);
#ifdef CONFIG_MVPP2_FPGA
	mv_pp2x_port_disable(pp);
#else
#ifdef CONFIG_MVPPV21
	mv_pp2x_port_disable(pp);
#else
	mv_gop110_port_events_mask(&pp->pp2->gop, &pp->mac_data);
	mv_gop110_port_disable(&pp->pp2->gop, &pp->mac_data);
#endif
#endif

	pp->txqs = kzalloc(txq_number * sizeof(struct mv_pp2x_tx_queue),
			   GFP_KERNEL);
	if (!pp->txqs)
		return -ENOMEM;

	/* U-Boot special: use preallocated area */
	pp->txqs[0].descs = buffer_loc.tx_descs;

	/* Initialize TX descriptor rings */
	for (queue = 0; queue < txq_number; queue++) {
		struct mv_pp2x_tx_queue *txq = &pp->txqs[queue];

		txq->id = mv_pp2x_txq_phys(pp->id, queue);
		txq->log_id = queue;
		txq->size = pp->tx_ring_size;
	}

	/* Init aggr txq, only cpu0 works in u-boot */
	pp->pp2->aggr_txqs = kzalloc(sizeof(struct mv_pp2x_tx_queue),
				GFP_KERNEL);
	if (!pp->pp2->aggr_txqs) {
		kfree(pp->txqs);
		return -ENOMEM;
	}

	pp->pp2->aggr_txqs->descs = buffer_loc.aggr_tx_descs;
	pp->pp2->aggr_txqs->id = 0;
	pp->pp2->aggr_txqs->log_id = 0;
	pp->pp2->aggr_txqs->size = MVPP2_AGGR_TXQ_SIZE;

	pp->rxqs = kzalloc(rxq_number * sizeof(struct mv_pp2x_rx_queue),
			   GFP_KERNEL);
	if (!pp->rxqs) {
		kfree(pp->txqs);
		kfree(pp->pp2->aggr_txqs);
		return -ENOMEM;
	}

	/* U-Boot special: use preallocated area */
	pp->rxqs[0].descs = buffer_loc.rx_descs;

	/* Create Rx descriptor rings */
	for (queue = 0; queue < rxq_number; queue++) {
		struct mv_pp2x_rx_queue *rxq = &pp->rxqs[queue];

		rxq->id = queue;
		rxq->size = pp->rx_ring_size;
	}

	mv_pp2x_ingress_enable(pp, false);

	/* Port default configuration */
	mv_pp2x_defaults_set(pp);

	return 0;
}

/* Device initialization routine */
static int mv_pp2x_probe(struct eth_device *dev)
{
	struct mv_pp2x_port *pp = dev->priv;
	int err;

	pp->tx_ring_size = MVPP2_MAX_TXD;
	pp->rx_ring_size = MVPP2_MAX_RXD;

	err = mv_pp2x_port_init(pp);
	if (err < 0) {
		printf("can't init pp2\n");
		return err;
	}

#ifndef CONFIG_MVPP2_FPGA
	mv_pp2x_write(pp->pp2, MVPP22_AXI_BM_WR_ATTR_REG,
				MVPP22_AXI_ATTR_SNOOP_CNTRL_BIT);
	mv_pp2x_write(pp->pp2, MVPP22_AXI_BM_RD_ATTR_REG,
				MVPP22_AXI_ATTR_SNOOP_CNTRL_BIT);
	mv_pp2x_write(pp->pp2, MVPP22_AXI_AGGRQ_DESCR_RD_ATTR_REG,
				MVPP22_AXI_ATTR_SNOOP_CNTRL_BIT);
	mv_pp2x_write(pp->pp2, MVPP22_AXI_TXQ_DESCR_WR_ATTR_REG,
				MVPP22_AXI_ATTR_SNOOP_CNTRL_BIT);
	mv_pp2x_write(pp->pp2, MVPP22_AXI_TXQ_DESCR_RD_ATTR_REG,
				MVPP22_AXI_ATTR_SNOOP_CNTRL_BIT);
	mv_pp2x_write(pp->pp2, MVPP22_AXI_RXQ_DESCR_WR_ATTR_REG,
				MVPP22_AXI_ATTR_SNOOP_CNTRL_BIT);
	mv_pp2x_write(pp->pp2, MVPP22_AXI_RX_DATA_WR_ATTR_REG,
				MVPP22_AXI_ATTR_SNOOP_CNTRL_BIT);
	mv_pp2x_write(pp->pp2, MVPP22_AXI_TX_DATA_RD_ATTR_REG,
				MVPP22_AXI_ATTR_SNOOP_CNTRL_BIT);
#endif

	/* Enable HW PHY polling */
#if !defined(CONFIG_MVPP2_FPGA) && defined(CONFIG_MVPPV21)
	u32 val;

	val = readl(pp->pp2->lms_base + MVPP2_PHY_AN_CFG0_REG);
	val &= ~MVPP2_PHY_AN_STOP_SMI0_MASK;
	writel(val, pp->pp2->lms_base + MVPP2_PHY_AN_CFG0_REG);
#endif

	/* Call open() now as it needs to be done before running send() */
	mv_pp2x_open(dev);

	return 0;
}

static int mv_pp2x_txq_drain_set(struct mv_pp2x_port *port, int txq, bool en)
{
	u32 reg_val;
	int ptxq = mv_pp2x_txq_phys(port->id, txq);

	mv_pp2x_write(port->pp2, MVPP2_TXQ_NUM_REG, ptxq);
	reg_val = mv_pp2x_read(port->pp2, MVPP2_TXQ_PREF_BUF_REG);

	if (en)
		reg_val |= MVPP2_TXQ_DRAIN_EN_MASK;
	else
		reg_val &= ~MVPP2_TXQ_DRAIN_EN_MASK;

	mv_pp2x_write(port->pp2, MVPP2_TXQ_PREF_BUF_REG, reg_val);

	return 0;
}

static int mv_pp2x_init_u_boot(struct eth_device *dev, bd_t *bis)
{
struct mv_pp2x_port *pp = dev->priv;

#ifdef CONFIG_PALLADIUM
	unsigned long auto_neg_value;
#endif /* CONFIG_PALLADIUM */

	if (!pp->init/* || pp->link == 0*/) {
		mv_pp2x_bm_start(pp->pp2);
		/* Full init on first call */
		mv_pp2x_probe(dev);
		/* Attach pool to rxq */
		mv_pp2x_rxq_bm_long_pool_set(pp, 0, MVPP2_BM_POOL);
		mv_pp2x_rxq_bm_short_pool_set(pp, 0, MVPP2_BM_POOL);
		/* mark this port being fully inited,
		 * otherwise it will be inited again
		 * during next networking transaction,
		 * including memory allocatation for
		 * TX/RX queue, PHY connect/configuration
		 * and address decode configuration.
		 */
		pp->init = 1;
	} else {
		/* Upon all following calls, this is enough */
		mv_pp2x_bm_start(pp->pp2);
		mv_pp2x_txq_drain_set(pp, 0, false);
#if defined(CONFIG_MVPP2_FPGA) || defined(CONFIG_MVPPV21)
		/* start the Rx/Tx activity */
		mv_pp2x_port_enable(pp);
#else
		mv_gop110_port_events_mask(&pp->pp2->gop, &pp->mac_data);
		mv_gop110_port_enable(&pp->pp2->gop, &pp->mac_data);
#endif
		mv_pp2x_ingress_enable(pp, true);
		mv_pp2x_egress_enable(pp);
	}

	return 0;
}

static void mv_pp2x_halt(struct eth_device *dev)
{
	struct mv_pp2x_port *pp = dev->priv;
/* Debug code for dump */

	mv_pp2x_bm_stop(pp->pp2);
	mv_pp2x_txq_drain_set(pp, 0, true);
	mv_pp2x_ingress_enable(pp, false);
	mv_pp2x_egress_disable(pp);

#if defined(CONFIG_MVPP2_FPGA) || defined(CONFIG_MVPPV21)
	mv_pp2x_port_disable(pp);
#else
	mv_gop110_port_events_mask(&pp->pp2->gop, &pp->mac_data);
	mv_gop110_port_disable(&pp->pp2->gop, &pp->mac_data);
#endif
}

/* Get number of sent descriptors and descrement counter.
   Clear sent descriptor counter.
   Number of sent descriptors is returned. */
static int mv_pp2x_txq_sent_desc_proc(struct mv_pp2x_port *port, int txq)
{
	u32  reg_val, ptxq;

	ptxq = mv_pp2x_txq_phys(port->id, txq);
	/* reading status reg also cause to reset transmitted counter */
	reg_val = mv_pp2x_read(port->pp2, MVPP22_TXQ_SENT_REG(ptxq));

	return (reg_val & MVPP22_TRANSMITTED_COUNT_MASK)
			>> MVPP22_TRANSMITTED_COUNT_OFFSET;
}

static inline void mv_pp2x1_txdesc_phys_addr_set(dma_addr_t phys_addr,
	struct mv_pp2x_tx_desc *tx_desc) {
	tx_desc->u.pp21.buf_phys_addr = phys_addr;
}

static inline void mv_pp2x2_txdesc_phys_addr_set(dma_addr_t phys_addr,
	struct mv_pp2x_tx_desc *tx_desc) {
	u64 *buf_phys_addr_p = &tx_desc->u.pp22.buf_phys_addr_hw_cmd2;

	*buf_phys_addr_p &= ~(MVPP22_ADDR_MASK);
	*buf_phys_addr_p |= phys_addr & MVPP22_ADDR_MASK;
}

static int mv_pp2x_send(struct eth_device *dev, void *ptr, int len)
{
	struct mv_pp2x_port *pp = dev->priv;
	struct mv_pp2x_tx_queue *aggr_txq = pp->pp2->aggr_txqs;
	struct mv_pp2x_tx_desc *tx_desc;
	int tx_done;
	u32 timeout = 0;

	tx_desc = mv_pp2x_txq_next_desc_get(aggr_txq);
	if (!tx_desc) {
		printf("No available descriptors\n");
		goto error;
	}

	/* set descriptor fields */
	tx_desc->command =  MVPP2_TXD_IP_CSUM_DISABLE |
		MVPP2_TXD_L4_CSUM_NOT | MVPP2_TXD_F_DESC | MVPP2_TXD_L_DESC;
	tx_desc->data_size = len;

	tx_desc->packet_offset = (phys_addr_t)ptr & MVPP2TX_DESC_ALIGN;

#ifdef CONFIG_MVPPV22
	mv_pp2x2_txdesc_phys_addr_set(
	(phys_addr_t)ptr & ~MVPP2TX_DESC_ALIGN, tx_desc);
#else
	mv_pp2x1_txdesc_phys_addr_set(
	(phys_addr_t)ptr & ~MVPP2TX_DESC_ALIGN, tx_desc);
#endif
	tx_desc->phys_txq = mv_pp2x_txq_phys(pp->id, 0);

	/* send */
	__iowmb();
	mv_pp2x_aggr_txq_pend_desc_add(pp, 1);

	/* Tx done processing */
	/* wait for agrregated to physical TXQ transfer */
	tx_done = mv_pp2x_aggr_txq_pend_desc_num_get(pp->pp2, 0);
	do {
		if (timeout++ > MVPP2_TX_SEND_TIMEOUT) {
			printf("timeout: packet not sent from aggr TXQ\n");
			goto error;
		}
		tx_done = mv_pp2x_aggr_txq_pend_desc_num_get(pp->pp2, 0);
	} while (tx_done);

	timeout = 0;
	tx_done = mv_pp2x_txq_sent_desc_proc(pp, 0);
	/* wait for packet to be transmitted */
	while (!tx_done) {
		if (timeout++ > MVPP2_TX_SEND_TIMEOUT) {
			printf("timeout: packet not sent\n");
			goto error;
		}
		tx_done = mv_pp2x_txq_sent_desc_proc(pp, 0);
	}
	/* tx_done has increased - hw sent packet */

	return 0;

error:
	printf("%s: %s failed\n", __func__, dev->name);

	return 1;
}

static int mv_pp2x_recv(struct eth_device *dev)
{
	struct mv_pp2x_port *pp = dev->priv;
	int num_received_packets, packets_done, pool_id;
	struct mv_pp2x_rx_desc *rx_desc;
	u8 *pkt;
	u32 status;
	u64 phy_addr, virt_addr;
	struct mv_pp2x_rx_queue *rxq = &pp->rxqs[0];

	num_received_packets = mv_pp2x_rxq_received(pp, 0);
	packets_done = num_received_packets;

	while (num_received_packets--) {
		rx_desc = mv_pp2x_rxq_next_desc_get(rxq);

		status = rx_desc->status;

		/* drop packets with error or with buffer header (MC, SG) */
		if ((status & MVPP2_RXD_BUF_HDR) ||
			(status & MVPP2_RXD_ERR_SUMMARY))
			continue;

		/* give packet to stack - skip on first
		  * 2 bytes + buffer header */
#ifdef CONFIG_MVPPV22
		phy_addr = rx_desc->u.pp22.buf_phys_addr_key_hash &
					MVPP22_ADDR_MASK;
		virt_addr = rx_desc->u.pp22.buf_cookie_bm_qset_cls_info &
					MVPP22_ADDR_MASK;
#else
		phy_addr = rx_desc->u.pp21.buf_phys_addr;
		virt_addr = rx_desc->u.pp21.buf_cookie;
#endif

		pkt = (u8 *)((uintptr_t)phy_addr) + 2 + BUFF_HDR_OFFS;
		NetReceive(pkt, (int)rx_desc->data_size - 2);

		/* refill: pass packet back to BM */
		pool_id = (status & MVPP2_RXD_BM_POOL_ID_MASK) >>
					MVPP2_RXD_BM_POOL_ID_OFFS;
		mv_pp2x_bm_pool_put(pp->pp2, pool_id, phy_addr, virt_addr);
	}

	__iowmb();
	mv_pp2x_rxq_desc_num_update(pp, 0, packets_done, packets_done);

	return 0;
}

static void mv_pp2x_mac_str_to_hex(const char *mac_str, unsigned char *mac_hex)
{
	int i;
	char tmp[3];

	for (i = 0; i < 6; i++) {
		tmp[0] = mac_str[(i * 3) + 0];
		tmp[1] = mac_str[(i * 3) + 1];
		tmp[2] = '\0';
		mac_hex[i] = (unsigned char)(simple_strtoul(tmp, NULL, 16));
	}
}

int mv_pp2x_phylib_init(struct eth_device *dev, int phyid, int gop_index)
{
	struct mii_dev *bus;
	struct phy_device *phydev;
	struct mv_pp2x_port *pp = dev->priv;

	bus = mdio_get_current_dev();
	if (!bus) {
		printf("mdio_alloc failed\n");
		return -ENOMEM;
	}
	sprintf(bus->name, dev->name);

	/* Set phy address of the port */
	mv_gop110_smi_phy_addr_cfg(&pp->pp2->gop, gop_index, phyid);

#if defined(CONFIG_PHYLIB)
	phydev = phy_connect(bus, phyid, dev, pp->mac_data.phy_mode);
	pp->mac_data.phy_dev = phydev;
	pp->bus = bus;
	if (!phydev) {
		printf("phy_connect failed dev->name(%s) phyid(%d)\n",
				dev->name, phyid);
		return -ENODEV;
	}
	phy_config(phydev);
	phy_startup(phydev);
	return 1;
#elif defined(CONFIG_MII) || defined(CONFIG_CMD_MII)
			miiphy_register(dev->name, bus->read, bus->write);
			/* Set phy address of the port */
			if (miiphy_write(dev->name, MV_PHY_ADR_REQUEST,
					 MV_PHY_ADR_REQUEST,
					 PHY_BASE_ADR + devnum) != 0) {
				netdev_err(dev, "miiphy write failed\n");
				return 1;
			}
#endif

	return 0;
}

/* RFU1 Functions  */
static inline u32 mv_gop110_rfu1_read(struct gop_hw *gop, u32 offset)
{
	return mv_gop_gen_read(gop->gop_110.rfu1_base, offset);
}

static inline void mv_gop110_rfu1_write(struct gop_hw *gop, u32 offset,
		u32 data)
{
	mv_gop_gen_write(gop->gop_110.rfu1_base, offset, data);
}

static inline void mv_gop110_rfu1_print(struct gop_hw *gop, char *reg_name,
		u32 reg)
{
	printf("  %-32s: 0x%x = 0x%08x\n", reg_name, reg,
	mv_gop110_rfu1_read(gop, reg));
}

static u32 mvp_pp2x_gop110_netc_cfg_create(struct mv_pp2x_port *pp2_port)
{
	u32 val = 0;

		if (pp2_port->mac_data.gop_index == 0) {
			if (pp2_port->mac_data.phy_mode ==
				PHY_INTERFACE_MODE_XAUI)
				val |= MV_NETC_GE_MAC0_XAUI;
			else if (pp2_port->mac_data.phy_mode ==
				PHY_INTERFACE_MODE_RXAUI)
				val |= MV_NETC_GE_MAC0_RXAUI_L23;
		}
		if (pp2_port->mac_data.gop_index == 2) {
			if (pp2_port->mac_data.phy_mode ==
				PHY_INTERFACE_MODE_SGMII)
				val |= MV_NETC_GE_MAC2_SGMII;
		}
		if (pp2_port->mac_data.gop_index == 3) {
			if (pp2_port->mac_data.phy_mode ==
				PHY_INTERFACE_MODE_SGMII)
				val |= MV_NETC_GE_MAC3_SGMII;
			else if (pp2_port->mac_data.phy_mode ==
				PHY_INTERFACE_MODE_RGMII)
				val |= MV_NETC_GE_MAC3_RGMII;
		}

	return val;
}

void mv_gop110_netc_active_port(struct gop_hw *gop, u32 port, u32 val)
{
	u32 reg;

	reg = mv_gop110_rfu1_read(gop, MV_NETCOMP_PORTS_CONTROL_1);
	reg &= ~(NETC_PORTS_ACTIVE_MASK(port));

	val <<= NETC_PORTS_ACTIVE_OFFSET(port);
	val &= NETC_PORTS_ACTIVE_MASK(port);

	reg |= val;

	mv_gop110_rfu1_write(gop, MV_NETCOMP_PORTS_CONTROL_1, reg);
}

static void mv_gop110_netc_xaui_enable(struct gop_hw *gop, u32 port, u32 val)
{
	u32 reg;

	reg = mv_gop110_rfu1_read(gop, SD1_CONTROL_1_REG);
	reg &= ~SD1_CONTROL_XAUI_EN_MASK;

	val <<= SD1_CONTROL_XAUI_EN_OFFSET;
	val &= SD1_CONTROL_XAUI_EN_MASK;

	reg |= val;

	mv_gop110_rfu1_write(gop, SD1_CONTROL_1_REG, reg);
}

static void mv_gop110_netc_rxaui0_enable(struct gop_hw *gop, u32 port, u32 val)
{
	u32 reg;

	reg = mv_gop110_rfu1_read(gop, SD1_CONTROL_1_REG);
	reg &= ~SD1_CONTROL_RXAUI0_L23_EN_MASK;

	val <<= SD1_CONTROL_RXAUI0_L23_EN_OFFSET;
	val &= SD1_CONTROL_RXAUI0_L23_EN_MASK;

	reg |= val;

	mv_gop110_rfu1_write(gop, SD1_CONTROL_1_REG, reg);
}

static void mv_gop110_netc_rxaui1_enable(struct gop_hw *gop, u32 port, u32 val)
{
	u32 reg;

	reg = mv_gop110_rfu1_read(gop, SD1_CONTROL_1_REG);
	reg &= ~SD1_CONTROL_RXAUI1_L45_EN_MASK;

	val <<= SD1_CONTROL_RXAUI1_L45_EN_OFFSET;
	val &= SD1_CONTROL_RXAUI1_L45_EN_MASK;

	reg |= val;

	mv_gop110_rfu1_write(gop, SD1_CONTROL_1_REG, reg);
}

static void mv_gop110_netc_mii_mode(struct gop_hw *gop, u32 port, u32 val)
{
	u32 reg;

	reg = mv_gop110_rfu1_read(gop, MV_NETCOMP_CONTROL_0);
	reg &= ~NETC_GBE_PORT1_MII_MODE_MASK;

	val <<= NETC_GBE_PORT1_MII_MODE_OFFSET;
	val &= NETC_GBE_PORT1_MII_MODE_MASK;

	reg |= val;

	mv_gop110_rfu1_write(gop, MV_NETCOMP_CONTROL_0, reg);
}

static void mv_gop110_netc_gop_reset(struct gop_hw *gop, u32 val)
{
	u32 reg;

	reg = mv_gop110_rfu1_read(gop, MV_GOP_SOFT_RESET_1_REG);
	reg &= ~NETC_GOP_SOFT_RESET_MASK;

	val <<= NETC_GOP_SOFT_RESET_OFFSET;
	val &= NETC_GOP_SOFT_RESET_MASK;

	reg |= val;

	mv_gop110_rfu1_write(gop, MV_GOP_SOFT_RESET_1_REG, reg);
}

static void mv_gop110_netc_gop_clock_logic_set(struct gop_hw *gop, u32 val)
{
	u32 reg;

	reg = mv_gop110_rfu1_read(gop, MV_NETCOMP_PORTS_CONTROL_0);
	reg &= ~NETC_CLK_DIV_PHASE_MASK;

	val <<= NETC_CLK_DIV_PHASE_OFFSET;
	val &= NETC_CLK_DIV_PHASE_MASK;

	reg |= val;

	mv_gop110_rfu1_write(gop, MV_NETCOMP_PORTS_CONTROL_0, reg);
}

static void mv_gop110_netc_port_rf_reset(struct gop_hw *gop, u32 port, u32 val)
{
	u32 reg;

	reg = mv_gop110_rfu1_read(gop, MV_NETCOMP_PORTS_CONTROL_1);
	reg &= ~(NETC_PORT_GIG_RF_RESET_MASK(port));

	val <<= NETC_PORT_GIG_RF_RESET_OFFSET(port);
	val &= NETC_PORT_GIG_RF_RESET_MASK(port);

	reg |= val;

	mv_gop110_rfu1_write(gop, MV_NETCOMP_PORTS_CONTROL_1, reg);
}

static void mv_gop110_netc_gbe_sgmii_mode_select(struct gop_hw *gop, u32 port,
						u32 val)
{
	u32 reg, mask, offset;

	if (port == 2) {
		mask = NETC_GBE_PORT0_SGMII_MODE_MASK;
		offset = NETC_GBE_PORT0_SGMII_MODE_OFFSET;
	} else {
		mask = NETC_GBE_PORT1_SGMII_MODE_MASK;
		offset = NETC_GBE_PORT1_SGMII_MODE_OFFSET;
	}
	reg = mv_gop110_rfu1_read(gop, MV_NETCOMP_CONTROL_0);
	reg &= ~mask;

	val <<= offset;
	val &= mask;

	reg |= val;

	mv_gop110_rfu1_write(gop, MV_NETCOMP_CONTROL_0, reg);
}

static void mv_gop110_netc_bus_width_select(struct gop_hw *gop, u32 val)
{
	u32 reg;

	reg = mv_gop110_rfu1_read(gop, MV_NETCOMP_PORTS_CONTROL_0);
	reg &= ~NETC_BUS_WIDTH_SELECT_MASK;

	val <<= NETC_BUS_WIDTH_SELECT_OFFSET;
	val &= NETC_BUS_WIDTH_SELECT_MASK;

	reg |= val;

	mv_gop110_rfu1_write(gop, MV_NETCOMP_PORTS_CONTROL_0, reg);
}

static void mv_gop110_netc_sample_stages_timing(struct gop_hw *gop, u32 val)
{
	u32 reg;

	reg = mv_gop110_rfu1_read(gop, MV_NETCOMP_PORTS_CONTROL_0);
	reg &= ~NETC_GIG_RX_DATA_SAMPLE_MASK;

	val <<= NETC_GIG_RX_DATA_SAMPLE_OFFSET;
	val &= NETC_GIG_RX_DATA_SAMPLE_MASK;

	reg |= val;

	mv_gop110_rfu1_write(gop, MV_NETCOMP_PORTS_CONTROL_0, reg);
}

static void mv_gop110_netc_mac_to_xgmii(struct gop_hw *gop, u32 port,
					enum mv_netc_phase phase)
{
	switch (phase) {
	case MV_NETC_FIRST_PHASE:
		/* Set Bus Width to HB mode = 1 */
		mv_gop110_netc_bus_width_select(gop, 1);
		/* Select RGMII mode */
		mv_gop110_netc_gbe_sgmii_mode_select(gop, port,
							MV_NETC_GBE_XMII);
		break;
	case MV_NETC_SECOND_PHASE:
		/* De-assert the relevant port HB reset */
		mv_gop110_netc_port_rf_reset(gop, port, 1);
		break;
	}
}

static void mv_gop110_netc_mac_to_sgmii(struct gop_hw *gop, u32 port,
					enum mv_netc_phase phase)
{
	switch (phase) {
	case MV_NETC_FIRST_PHASE:
		/* Set Bus Width to HB mode = 1 */
		mv_gop110_netc_bus_width_select(gop, 1);
		/* Select SGMII mode */
		if (port >= 1)
			mv_gop110_netc_gbe_sgmii_mode_select(gop, port,
			MV_NETC_GBE_SGMII);

		/* Configure the sample stages */
		mv_gop110_netc_sample_stages_timing(gop, 0);
		/* Configure the ComPhy Selector */
		/* mv_gop110_netc_com_phy_selector_config(netComplex); */
		break;
	case MV_NETC_SECOND_PHASE:
		/* De-assert the relevant port HB reset */
		mv_gop110_netc_port_rf_reset(gop, port, 1);
		break;
	}
}

static void mv_gop110_netc_mac_to_rxaui(struct gop_hw *gop, u32 port,
					enum mv_netc_phase phase,
					enum mv_netc_lanes lanes)
{
	/* Currently only RXAUI0 supported */
	if (port != 0)
		return;

	switch (phase) {
	case MV_NETC_FIRST_PHASE:
		/* RXAUI Serdes/s Clock alignment */
		if (lanes == MV_NETC_LANE_23)
			mv_gop110_netc_rxaui0_enable(gop, port, 1);
		else
			mv_gop110_netc_rxaui1_enable(gop, port, 1);
		break;
	case MV_NETC_SECOND_PHASE:
		/* De-assert the relevant port HB reset */
		mv_gop110_netc_port_rf_reset(gop, port, 1);
		break;
	}
}

static void mv_gop110_netc_mac_to_xaui(struct gop_hw *gop, u32 port,
					enum mv_netc_phase phase)
{
	switch (phase) {
	case MV_NETC_FIRST_PHASE:
		/* RXAUI Serdes/s Clock alignment */
		mv_gop110_netc_xaui_enable(gop, port, 1);
		break;
	case MV_NETC_SECOND_PHASE:
		/* De-assert the relevant port HB reset */
		mv_gop110_netc_port_rf_reset(gop, port, 1);
		break;
	}
}

int mv_gop110_netc_init(struct gop_hw *gop,
			u32 net_comp_config, enum mv_netc_phase phase)
{
	u32 c = net_comp_config;

	if (c & MV_NETC_GE_MAC0_RXAUI_L23)
		mv_gop110_netc_mac_to_rxaui(gop, 0, phase, MV_NETC_LANE_23);

	if (c & MV_NETC_GE_MAC0_RXAUI_L45)
		mv_gop110_netc_mac_to_rxaui(gop, 0, phase, MV_NETC_LANE_45);

	if (c & MV_NETC_GE_MAC0_XAUI)
		mv_gop110_netc_mac_to_xaui(gop, 0, phase);

	if (c & MV_NETC_GE_MAC2_SGMII)
		mv_gop110_netc_mac_to_sgmii(gop, 2, phase);
	else
		mv_gop110_netc_mac_to_xgmii(gop, 2, phase);
	if (c & MV_NETC_GE_MAC3_SGMII)
		mv_gop110_netc_mac_to_sgmii(gop, 3, phase);
	else {
		mv_gop110_netc_mac_to_xgmii(gop, 3, phase);
		if (c & MV_NETC_GE_MAC3_RGMII)
			mv_gop110_netc_mii_mode(gop, 3, MV_NETC_GBE_RGMII);
		else
			mv_gop110_netc_mii_mode(gop, 3, MV_NETC_GBE_MII);
	}

	/* Activate gop ports 0, 2, 3 */
	mv_gop110_netc_active_port(gop, 0, 1);
	mv_gop110_netc_active_port(gop, 2, 1);
	mv_gop110_netc_active_port(gop, 3, 1);

	if (phase == MV_NETC_SECOND_PHASE) {
		/* Enable the GOP internal clock logic */
		mv_gop110_netc_gop_clock_logic_set(gop, 1);
		/* De-assert GOP unit reset */
		mv_gop110_netc_gop_reset(gop, 1);
	}
	return 0;
}

#if !defined(CONFIG_MV_PP2_FPGA) && !defined(CONFIG_MV_PP2_PALLADIUM)
static int mvcpn110_mac_hw_init(struct mv_pp2x_port *port)
{
	struct gop_hw *gop = &port->pp2->gop;
	struct mv_mac_data *mac = &port->mac_data;

	mv_gop110_port_init(gop, mac);

	if (mac->force_link)
		mv_gop110_fl_cfg(gop, mac);

	mac->flags |= MV_EMAC_F_INIT;

	return 0;
}
#endif

void mv_pp2x_axi_config(struct mv_pp2x *pp2)
{
	/* Config AXI Read&Write Normal and Soop mode  */
	mv_pp2x_write(pp2, MVPP22_AXI_RD_NORMAL_CODE_REG,
				MVPP22_AXI_RD_CODE_MASK);
	mv_pp2x_write(pp2, MVPP22_AXI_RD_SNP_CODE_REG, MVPP22_AXI_RD_CODE_MASK);
	mv_pp2x_write(pp2, MVPP22_AXI_WR_NORMAL_CODE_REG,
				MVPP22_AXI_WR_CODE_MASK);
	mv_pp2x_write(pp2, MVPP22_AXI_WR_SNP_CODE_REG, MVPP22_AXI_WR_CODE_MASK);
}

#ifdef CONFIG_OF_CONTROL

DECLARE_GLOBAL_DATA_PTR;

char *phy_mode_str[] = {
	"mii",
	"gmii",
	"sgmii",
	"sgmii_2500",
	"qsgmii",
	"tbi",
	"rmii",
	"rgmii",
	"rgmii_id",
	"rgmii_rxid",
	"rgmii_txid",
	"rtbi",
	"xgmii",
	"none"	/* Must be last */
};

struct mv_pp2x_reg_info {
	u32 base;
	u32 size;
};

int mv_pp2x_initialize_dev(bd_t *bis, struct mv_pp2x *pp2,
						struct mv_pp2x_dev_para *para)
{
	struct eth_device *dev;
	struct mv_pp2x_port *pp2_port;
	void *bd_space;
	char *enet_addr;
	char enetvar[9];
	u32 net_comp_config;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL)
		return -ENOMEM;

	pp2_port = calloc(1, sizeof(*pp2_port));
	if (pp2_port == NULL)
		return -ENOMEM;

	pp2_port->id = para->dev_num;
	pp2_port->txp_num = 1;
	pp2_port->pp2 = pp2;
	pp2_port->base = para->base;
	dev->priv = pp2_port;
	pp2_port->mac_data.gop_index = para->gop_port;
	pp2_port->mac_data.phy_mode = para->phy_type;

	/*
	 * Allocate buffer area for tx/rx descs and rx_buffers. This is only
	 * done once for all interfaces. As only one interface can
	 * be active. Make this area DMA save by disabling the D-cache
	 */
	if (!buffer_loc.tx_descs) {
		/* Align buffer area for descs and rx_buffers to 1MiB */
		bd_space = memalign(MVPP2_BUFFER_ALIGN_SIZE, BD_SPACE);

		if (bd_space == NULL)
			return -ENOMEM;

#ifndef CONFIG_SYS_DCACHE_OFF /* Uboot cache always off */
		mmu_set_region_dcache_behaviour((u32)bd_space, BD_SPACE,
						DCACHE_OFF);
#endif
		buffer_loc.tx_descs = (struct mv_pp2x_tx_desc *)bd_space;

		buffer_loc.aggr_tx_descs = (struct mv_pp2x_tx_desc *)
			((unsigned long)bd_space + MVPP2_MAX_TXD
			* sizeof(struct mv_pp2x_tx_desc));

		buffer_loc.rx_descs = (struct mv_pp2x_rx_desc *)
			((unsigned long)bd_space +
			(MVPP2_MAX_TXD + MVPP2_AGGR_TXQ_SIZE)
			* sizeof(struct mv_pp2x_tx_desc));

		buffer_loc.rx_buffers = (unsigned long)
			(bd_space + (MVPP2_MAX_TXD + MVPP2_AGGR_TXQ_SIZE)
			* sizeof(struct mv_pp2x_tx_desc) +
			 MVPP2_MAX_RXD * sizeof(struct mv_pp2x_rx_desc));
	}

	/* interface name */
	sprintf(dev->name, "egiga%d", pp2_port->id);
	/* interface MAC addr extract */
	sprintf(enetvar, para->dev_num ? "eth%daddr" :
			"ethaddr", pp2_port->id);
	enet_addr = getenv(enetvar);
	mv_pp2x_mac_str_to_hex(enet_addr, (unsigned char *)(dev->enetaddr));

	dev->iobase = (unsigned long)pp2_port->base;
	dev->init = mv_pp2x_init_u_boot;
	dev->halt = mv_pp2x_halt;
	dev->send = mv_pp2x_send;
	dev->recv = mv_pp2x_recv;
	dev->write_hwaddr = NULL;
	dev->index = pp2_port->id;
#ifdef CONFIG_PALLADIUM
	/* on Palladium, there is no mac address in env,
	 * so put a value to skip the validation  otherwise
	 *u-boot would fail at common net driver validation.
	 */
	dev->enetaddr[1] = 51;
#endif
	/*
	 * The PHY interface type is configured via the
	 * board specific CONFIG_SYS_NETA_INTERFACE_TYPE
	 * define.
	 */
	pp2_port->mac_data.phy_mode = para->phy_type;
	pp2_port->mac_data.phy_addr = para->phy_addr;

	eth_register(dev);

	/* netcomp  Init  */
	net_comp_config = mvp_pp2x_gop110_netc_cfg_create(pp2_port);
	mv_gop110_netc_init(&pp2_port->pp2->gop, net_comp_config,
				MV_NETC_FIRST_PHASE);
	mv_gop110_netc_init(&pp2_port->pp2->gop, net_comp_config,
				MV_NETC_SECOND_PHASE);

	/* GOP Init  */
	mvcpn110_mac_hw_init(pp2_port);
	mv_pp2x_phylib_init(dev, para->phy_addr, para->gop_port);

	return 1;
}

/* get all configuration from FDT file, no finish yet!!!!!!! */
int mv_pp2x_initialize(bd_t *bis)
{
	int mv_pp2x_node_list[CONFIG_MAX_MVPP2X_NUM], node, port_node;
	int pp2_count, emac_off, phy_off, port_id, gop_port, mdio_phy;
	int phy_mode = 0;
	struct mv_pp2x *pp2;
	struct mv_pp2x_dev_para dev_para[CONFIG_MAX_PP2_PORT_NUM];
	int err;
	u32 *emac_handle, *phy_handle;
	char *phy_mode_str;

	/* in dts file, go through all the 'pp2' nodes.
	 */
	pp2_count = fdtdec_find_aliases_for_id(gd->fdt_blob, "pp2",
			COMPAT_MVEBU_PP2, mv_pp2x_node_list, 2);
	if (pp2_count == 0) {
		printf(
		"could not find pp2 node in FDT, initialization skipped!\n");
		return 0;
	}

	pp2 = calloc(1, sizeof(*pp2));
	if (pp2 == NULL)
		return -ENOMEM;

#ifdef CONFIG_MVPPV21
		pp2->gop.lms_base =
		(unsigned long)fdt_get_regs_offs(gd->fdt_blob, node, "lms_reg");
		if (pp2->gop.lms_base == FDT_ADDR_T_NONE) {
			printf(
			"could not find reg in pp2 node, initialization skipped!\n");
			return 0;
		}
#endif

		node = mv_pp2x_node_list[pp2_count - 1];

		pp2->base =
			(void *)fdt_get_regs_offs(gd->fdt_blob,
							 node, "pp");
		if (pp2->base == 0) {
			printf(
			"could not find base reg in pp2 node, init skipped!\n");
		}
		pp2->gop.gop_110.serdes.base =
			(void *)fdt_get_regs_offs(gd->fdt_blob,
							 node, "serdes");
		if (pp2->gop.gop_110.serdes.base == 0) {
			printf(
			"could not find serdes reg in pp2 node, init skipped!\n");
		}
		pp2->gop.gop_110.serdes.obj_size = 0x1000;
		pp2->gop.gop_110.xmib.base =
			(void *)fdt_get_regs_offs(gd->fdt_blob,
							 node, "xmib");
		if (pp2->gop.gop_110.xmib.base == 0) {
			printf(
			"could not find xmib reg in pp2 node, init skipped!\n");
		}
		pp2->gop.gop_110.xmib.obj_size = 0x0100;
		pp2->gop.gop_110.smi_base =
			(void *)fdt_get_regs_offs(gd->fdt_blob,
							 node, "smi");
		if (pp2->gop.gop_110.smi_base == 0) {
			printf(
			"could not find smi reg in pp2 node, init skipped!\n");
		}
		pp2->gop.gop_110.xsmi_base =
			(void *)fdt_get_regs_offs(gd->fdt_blob,
							 node, "xsmi");
		if (pp2->gop.gop_110.xsmi_base == 0) {
			printf(
			"could not find xsmi reg in pp2 node, init skipped!\n");
		}
		pp2->gop.gop_110.mspg_base =
			(void *)fdt_get_regs_offs(gd->fdt_blob,
							 node, "mspg");
		if (pp2->gop.gop_110.mspg_base == 0) {
			printf(
			"could not find mspg reg in pp2 node, init skipped!\n");
		}
		pp2->gop.gop_110.xpcs_base =
			(void *)fdt_get_regs_offs(gd->fdt_blob,
							 node, "xpcs");
		if (pp2->gop.gop_110.xpcs_base == 0) {
			printf(
			"could not find xpcs reg in pp2 node, init skipped!\n");
		}
		pp2->gop.gop_110.gmac.base =
			(void *)fdt_get_regs_offs(gd->fdt_blob,
							 node, "gmac");
		if (pp2->gop.gop_110.gmac.base == 0) {
			printf(
			"could not find gmac reg in pp2 node, init skipped!\n");
		}
		pp2->gop.gop_110.gmac.obj_size = 0x1000;
		pp2->gop.gop_110.xlg_mac.base =
			(void *)fdt_get_regs_offs(gd->fdt_blob,
							 node, "xlg");
		if (pp2->gop.gop_110.xlg_mac.base == 0) {
			printf(
			"could not find xlg reg in pp2 node, init skipped!\n");
		}
		pp2->gop.gop_110.xlg_mac.obj_size = 0x1000;
		pp2->gop.gop_110.rfu1_base =
			(void *)fdt_get_regs_offs(gd->fdt_blob,
							 node, "rfu1");
		if (pp2->gop.gop_110.rfu1_base == 0) {
			printf(
			"could not find rfu1 reg in pp2 node, init skipped!\n");
		}

		/* AXI config */
		mv_pp2x_axi_config(pp2);

		/* Init BM */
		mv_pp2x_bm_pool_init(pp2);

		/* Rx Fifo Init */
		mv_pp2x_rx_fifo_init(pp2);

		/* Parser Init */
		err = mv_pp2x_prs_default_init(pp2);
		if (err) {
			printf("Parser init error\n");
			return -1;
		}

		/* Cls Init */
		err = mv_pp2x_cls_default_init(pp2);
		if (err) {
			printf("Cls init error\n");
			return -1;
		}

		fdt_for_each_subnode(gd->fdt_blob, port_node, node) {
			port_id = (uintptr_t)fdtdec_get_int(gd->fdt_blob,
						port_node, "port-id", 0);

			emac_handle = (u32 *)fdt_getprop(gd->fdt_blob,
						port_node, "emac-data", NULL);
			if (!emac_handle) {
				printf("no emac-data property\n");
				return -1;
			}

			emac_off = fdt_node_offset_by_phandle(gd->fdt_blob,
				  fdt32_to_cpu(*emac_handle));
			if (emac_off < 0) {
				printf("%s: %s\n", __func__, fdt_strerror(emac_off));
				return -1;
			}

			gop_port = (uintptr_t)fdtdec_get_int(gd->fdt_blob,
						emac_off, "port-id", 0);

			phy_mode_str = (void *)fdt_getprop(gd->fdt_blob, emac_off,
						   "phy-mode", NULL);
			if (strncmp(phy_mode_str, "sgmii", 5) == 0)
				phy_mode = PHY_INTERFACE_MODE_SGMII;
			else if (strncmp(phy_mode_str, "rgmii", 5) == 0)
				phy_mode = PHY_INTERFACE_MODE_RGMII;

			if (phy_mode != PHY_INTERFACE_MODE_SGMII &&
				phy_mode != PHY_INTERFACE_MODE_RGMII) {
				printf(
				"could not find phy-mode in pp2 node, init skipped!\n");
			}

			/*skip if port is configured not to use */
			if (phy_mode == PHY_INTERFACE_MODE_RGMII) {
				phy_handle = (u32 *)fdt_getprop(gd->fdt_blob,
							emac_off, "phy", NULL);
				if (!phy_handle) {
					printf("no phy property\n");
					return -1;
				}

				phy_off = fdt_node_offset_by_phandle(gd->fdt_blob,
					  fdt32_to_cpu(*phy_handle));
				if (phy_off < 0) {
					printf("could not find phy address\n");
					return -1;
				}

				mdio_phy = (uintptr_t)fdtdec_get_int(gd->fdt_blob,
							phy_off, "reg", 0);
				if (mdio_phy < 0) {
					printf("could not find mdio phy address\n");
					return -1;
				}
				dev_para[port_id].dev_num = port_id;
				dev_para[port_id].base = pp2->base;
				dev_para[port_id].phy_addr = mdio_phy;
				dev_para[port_id].phy_type = phy_mode;
				dev_para[port_id].gop_port = gop_port;
				if (1 != mv_pp2x_initialize_dev(bis,
					pp2, &dev_para[port_id])) {
					printf(
					"mv_pp2x_initialize_dev failed, initialization skipped!\n");
					return -1;
				}
			}
		}

	return 0;
}

#endif /* CONFIG_MVPP2_FPGA */
