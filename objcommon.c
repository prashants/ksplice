/*  Copyright (C) 2007-2008  Jeffrey Brian Arnold <jbarnold@mit.edu>
 *  Copyright (C) 2008  Anders Kaseorg <andersk@mit.edu>,
 *                      Tim Abbott <tabbott@mit.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License, version 2.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston, MA
 *  02110-1301, USA.
 */

#define _GNU_SOURCE
#include "objcommon.h"
#include <stdio.h>

void vec_do_reserve(void **data, size_t *mem_size, size_t new_size)
{
	if (new_size > *mem_size || new_size * 2 < *mem_size) {
		if (new_size < *mem_size * 2)
			new_size = *mem_size * 2;
		*data = realloc(*data, new_size);
		assert(new_size == 0 || *data != NULL);
		*mem_size = new_size;
	}
}

void get_syms(bfd *abfd, struct asymbolp_vec *syms)
{
	long storage_needed = bfd_get_symtab_upper_bound(abfd);
	if (storage_needed == 0)
		return;
	assert(storage_needed >= 0);

	vec_init(syms);
	vec_reserve(syms, storage_needed);
	vec_resize(syms, bfd_canonicalize_symtab(abfd, syms->data));
	assert(syms->size >= 0);
}

struct superbfd *fetch_superbfd(bfd *abfd)
{
	assert(abfd != NULL);
	if (abfd->usrdata != NULL)
		return abfd->usrdata;

	struct superbfd *sbfd = malloc(sizeof(*sbfd));
	assert(sbfd != NULL);

	abfd->usrdata = sbfd;
	sbfd->abfd = abfd;
	get_syms(abfd, &sbfd->syms);
	sbfd->new_supersects = NULL;
	return sbfd;
}

struct supersect *fetch_supersect(struct superbfd *sbfd, asection *sect)
{
	assert(sect != NULL);
	if (sect->userdata != NULL)
		return sect->userdata;

	struct supersect *new = malloc(sizeof(*new));
	assert(new != NULL);

	sect->userdata = new;
	new->parent = sbfd;
	new->name = sect->name;
	new->flags = bfd_get_section_flags(sbfd->abfd, sect);

	vec_init(&new->contents);
	vec_resize(&new->contents, bfd_get_section_size(sect));
	assert(bfd_get_section_contents
	       (sbfd->abfd, sect, new->contents.data, 0, new->contents.size));
	new->alignment = bfd_get_section_alignment(sbfd->abfd, sect);

	vec_init(&new->relocs);
	vec_reserve(&new->relocs, bfd_get_reloc_upper_bound(sbfd->abfd, sect));
	vec_resize(&new->relocs,
		   bfd_canonicalize_reloc(sbfd->abfd, sect, new->relocs.data,
					  sbfd->syms.data));
	assert(new->relocs.size >= 0);
	vec_init(&new->new_relocs);

	return new;
}

struct supersect *new_supersect(struct superbfd *sbfd, const char *name)
{
	struct supersect *ss;
	for (ss = sbfd->new_supersects; ss != NULL; ss = ss->next) {
		if (strcmp(name, ss->name) == 0)
			return ss;
	}

	struct supersect *new = malloc(sizeof(*new));
	new->parent = sbfd;
	new->name = name;
	new->next = sbfd->new_supersects;
	sbfd->new_supersects = new;
	new->flags = SEC_ALLOC | SEC_HAS_CONTENTS | SEC_RELOC;

	vec_init(&new->contents);
	new->alignment = 0;
	vec_init(&new->relocs);
	vec_init(&new->new_relocs);

	return new;
}

void supersect_move(struct supersect *dest_ss, struct supersect *src_ss)
{
	*dest_ss = *src_ss;
	vec_init(&src_ss->contents);
	vec_init(&src_ss->relocs);
	vec_init(&src_ss->new_relocs);
}

void *sect_do_grow(struct supersect *ss, size_t n, size_t size, int alignment)
{
	if (ss->alignment < ffs(alignment) - 1)
		ss->alignment = ffs(alignment) - 1;
	int pad = align(ss->contents.size, alignment) - ss->contents.size;
	void *out = vec_grow(&ss->contents, pad + n * size);
	memset(out, 0, pad + n * size);
	return out + pad;
}

static void mod_relocs(struct arelentp_vec *dest_relocs,
		       struct arelentp_vec *src_relocs,
		       bfd_size_type start, bfd_size_type end,
		       bfd_size_type mod)
{
	arelent **relocp;
	for (relocp = src_relocs->data;
	     relocp < src_relocs->data + src_relocs->size; relocp++) {
		if ((*relocp)->address >= start && (*relocp)->address < end) {
			arelent *reloc = malloc(sizeof(*reloc));
			assert(reloc != NULL);
			*reloc = **relocp;
			reloc->address += mod;
			*vec_grow(dest_relocs, 1) = reloc;
		}
	}
}

void sect_do_copy(struct supersect *dest_ss, void *dest,
		  struct supersect *src_ss, const void *src, size_t n)
{
	memcpy(dest, src, n);
	bfd_size_type start = src - src_ss->contents.data;
	bfd_size_type end = start + n;
	bfd_size_type mod = (dest - dest_ss->contents.data) -
	    (src - src_ss->contents.data);
	mod_relocs(&dest_ss->relocs, &src_ss->relocs, start, end, mod);
	mod_relocs(&dest_ss->new_relocs, &src_ss->new_relocs, start, end, mod);
}

bfd_vma addr_offset(struct supersect *ss, const void *addr)
{
	return (void *)addr - ss->contents.data;
}

bfd_vma get_reloc_offset(struct supersect *ss, arelent *reloc, int adjust_pc)
{
	int size = bfd_get_reloc_size(reloc->howto);

	bfd_vma x = bfd_get(size * 8, ss->parent->abfd,
			    ss->contents.data + reloc->address);
	x &= reloc->howto->src_mask;
	x >>= reloc->howto->bitpos;
	bfd_vma signbit = reloc->howto->dst_mask >> reloc->howto->bitpos;
	signbit &= ~(signbit >> 1);
	switch (reloc->howto->complain_on_overflow) {
	case complain_overflow_signed:
	case complain_overflow_bitfield:
		x |= -(x & signbit);
		break;
	case complain_overflow_unsigned:
		break;
	default:
		DIE;
	}
	x <<= reloc->howto->rightshift;

	bfd_vma add = reloc->addend;
	if (reloc->howto->pc_relative) {
		if (!reloc->howto->pcrel_offset)
			add += reloc->address;
		if (adjust_pc)
			add += size;
	}
	return x + add;
}

bfd_vma read_reloc(struct supersect *ss, const void *addr, size_t size,
		   asymbol **symp)
{
	arelent **relocp;
	bfd_vma val = bfd_get(size * 8, ss->parent->abfd, addr);
	bfd_vma address = addr_offset(ss, addr);
	for (relocp = ss->relocs.data;
	     relocp < ss->relocs.data + ss->relocs.size; relocp++) {
		arelent *reloc = *relocp;
		if (reloc->address == address) {
			if (symp != NULL)
				*symp = *reloc->sym_ptr_ptr;
			else if (*reloc->sym_ptr_ptr !=
				 bfd_abs_section_ptr->symbol)
				fprintf(stderr, "warning: unexpected "
					"non-absolute relocation at %s+%lx\n",
					ss->name, (unsigned long)address);
			return get_reloc_offset(ss, reloc, 0);
		}
	}
	if (symp != NULL)
		*symp = *bfd_abs_section_ptr->symbol_ptr_ptr;
	return val;
}

char *str_pointer(struct supersect *ss, void *const *addr)
{
	asymbol *sym;
	bfd_vma offset = read_reloc(ss, addr, sizeof(*addr), &sym);
	char *str;
	assert(asprintf(&str, "%s+%lx", sym->name, (unsigned long)offset) >= 0);
	return str;
}

const void *read_pointer(struct supersect *ss, void *const *addr,
			 struct supersect **data_ssp)
{
	asymbol *sym;
	bfd_vma offset = read_reloc(ss, addr, sizeof(*addr), &sym);
	struct supersect *data_ss = fetch_supersect(ss->parent, sym->section);
	if (bfd_is_abs_section(sym->section) && sym->value + offset == 0)
		return NULL;
	if (bfd_is_const_section(sym->section)) {
		fprintf(stderr, "warning: unexpected relocation to const "
			"section at %s+%lx\n", data_ss->name,
			(unsigned long)addr_offset(data_ss, addr));
		return NULL;
	}
	if (data_ssp != NULL)
		*data_ssp = data_ss;
	return data_ss->contents.data + sym->value + offset;
}

const char *read_string(struct supersect *ss, const char *const *addr)
{
	return read_pointer(ss, (void *const *)addr, NULL);
}
