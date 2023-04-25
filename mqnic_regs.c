/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2023 Xinyu Yang.
 * Copyright 2021, The Regents of the University of California.
 * All rights reserved.
 */

#include "mqnic.h"

struct mqnic_reg_block *mqnic_enumerate_reg_block_list(u8 *addr, size_t offset, size_t size)
{
	int max_count = 8;
	struct mqnic_reg_block *reg_block_list = rte_malloc("mqnic register block", max_count * sizeof(struct mqnic_reg_block), 0);
	int count = 0;
	int k;

	u8 *ptr;

	u32 rb_type;
	u32 rb_version;

	if (!reg_block_list)
		return NULL;

	while (1) {
		reg_block_list[count].type = 0;
		reg_block_list[count].version = 0;
		reg_block_list[count].base = 0;
		reg_block_list[count].regs = 0;

		if ((offset == 0 && count != 0) || offset >= size)
			break;

		ptr = addr + offset;

		for (k = 0; k < count; k++)
		{
			if (ptr == reg_block_list[k].regs)
			{
				PMD_INIT_LOG(ERR, "Register blocks form a loop");
				goto fail;
			}
		}

		rb_type = MQNIC_DIRECT_READ_REG(ptr, MQNIC_RB_REG_TYPE);
		rb_version = MQNIC_DIRECT_READ_REG(ptr, MQNIC_RB_REG_VER);
		offset = MQNIC_DIRECT_READ_REG(ptr, MQNIC_RB_REG_NEXT_PTR);

		reg_block_list[count].type = rb_type;
		reg_block_list[count].version = rb_version;
		reg_block_list[count].base = addr;
		reg_block_list[count].regs = ptr;

		count++;

		if (count >= max_count) {
			struct mqnic_reg_block *tmp;
			max_count += 4;
			tmp = rte_malloc("mqnic register block", max_count * sizeof(struct mqnic_reg_block), 0);
			if (!tmp)
				goto fail;
			reg_block_list = tmp;
		}
	}

	return reg_block_list;
fail:
	rte_free(reg_block_list);
	return NULL;
}

struct mqnic_reg_block *mqnic_find_reg_block(struct mqnic_reg_block *list, u32 type, u32 version, int index)
{
	struct mqnic_reg_block *rb = list;

	while (rb->regs) {
		if (rb->type == type && (!version || rb->version == version)) {
			if (index > 0)
				index--;
			else
				return rb;
		}

		rb++;
	}

	return NULL;
}

void mqnic_free_reg_block_list(struct mqnic_reg_block *list)
{
	rte_free(list);
}
