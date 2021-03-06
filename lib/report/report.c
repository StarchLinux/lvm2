/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2013 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "metadata.h"
#include "report.h"
#include "toolcontext.h"
#include "lvm-string.h"
#include "display.h"
#include "activate.h"
#include "segtype.h"
#include "lvmcache.h"

#include <stddef.h> /* offsetof() */

struct lvm_report_object {
	struct volume_group *vg;
	struct logical_volume *lv;
	struct physical_volume *pv;
	struct lv_segment *seg;
	struct pv_segment *pvseg;
};

static const uint64_t _minusone64 = UINT64_C(-1);
static const int32_t _minusone32 = INT32_C(-1);
static const uint64_t _zero64 = UINT64_C(0);

/*
 * Data-munging functions to prepare each data type for display and sorting
 */
static int _string_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
			struct dm_report_field *field,
			const void *data, void *private __attribute__((unused)))
{
	return dm_report_field_string(rh, field, (const char * const *) data);
}

static int _dev_name_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
			  struct dm_report_field *field,
			  const void *data, void *private __attribute__((unused)))
{
	const char *name = dev_name(*(const struct device * const *) data);

	return dm_report_field_string(rh, field, &name);
}

static int _devices_disp(struct dm_report *rh __attribute__((unused)), struct dm_pool *mem,
			 struct dm_report_field *field,
			 const void *data, void *private __attribute__((unused)))
{
	char *str;
	if (!(str = lvseg_devices(mem, (const struct lv_segment *) data)))
		return 0;

	dm_report_field_set_value(field, str, NULL);

	return 1;
}

static int _peranges_disp(struct dm_report *rh __attribute__((unused)), struct dm_pool *mem,
			  struct dm_report_field *field,
			  const void *data, void *private __attribute__((unused)))
{
	char *str;
	if (!(str = lvseg_seg_pe_ranges(mem, (const struct lv_segment *) data)))
		return 0;

	dm_report_field_set_value(field, str, NULL);

	return 1;
}

static int _tags_disp(struct dm_report *rh __attribute__((unused)), struct dm_pool *mem,
		      struct dm_report_field *field,
		      const void *data, void *private __attribute__((unused)))
{
	const struct dm_list *tags = (const struct dm_list *) data;
	char *tags_str;

	if (!(tags_str = tags_format_and_copy(mem, tags)))
		return 0;

	dm_report_field_set_value(field, tags_str, NULL);

	return 1;
}

static int _modules_disp(struct dm_report *rh, struct dm_pool *mem,
			 struct dm_report_field *field,
			 const void *data, void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	char *modules_str;

	if (!(modules_str = lv_modules_dup(mem, lv)))
		return 0;

	dm_report_field_set_value(field, modules_str, NULL);
	return 1;
}

static int _lvprofile_disp(struct dm_report *rh, struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data, void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;

	if (lv->profile)
		return dm_report_field_string(rh, field, &lv->profile->name);

	dm_report_field_set_value(field, "", NULL);
	return 1;
}

static int _vgfmt_disp(struct dm_report *rh, struct dm_pool *mem,
		       struct dm_report_field *field,
		       const void *data, void *private)
{
	const struct volume_group *vg = (const struct volume_group *) data;

	if (!vg->fid) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	return _string_disp(rh, mem, field, &vg->fid->fmt->name, private);
}

static int _pvfmt_disp(struct dm_report *rh, struct dm_pool *mem,
		       struct dm_report_field *field,
		       const void *data, void *private)
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;

	if (!pv->fmt) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	return _string_disp(rh, mem, field, &pv->fmt->name, private);
}

static int _lvkmaj_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
			struct dm_report_field *field,
			const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	int major;

	if ((major = lv_kernel_major(lv)) >= 0)
		return dm_report_field_int(rh, field, &major);

	return dm_report_field_int32(rh, field, &_minusone32);
}

static int _lvkmin_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
			struct dm_report_field *field,
			const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	int minor;

	if ((minor = lv_kernel_minor(lv)) >= 0)
		return dm_report_field_int(rh, field, &minor);

	return dm_report_field_int32(rh, field, &_minusone32);
}

static int _lvstatus_disp(struct dm_report *rh __attribute__((unused)), struct dm_pool *mem,
			  struct dm_report_field *field,
			  const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	char *repstr;

	if (!(repstr = lv_attr_dup(mem, lv)))
		return 0;

	dm_report_field_set_value(field, repstr, NULL);
	return 1;
}

static int _pvstatus_disp(struct dm_report *rh __attribute__((unused)), struct dm_pool *mem,
			  struct dm_report_field *field,
			  const void *data, void *private __attribute__((unused)))
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;
	char *repstr;

	if (!(repstr = pv_attr_dup(mem, pv)))
		return 0;

	dm_report_field_set_value(field, repstr, NULL);
	return 1;
}

static int _vgstatus_disp(struct dm_report *rh __attribute__((unused)), struct dm_pool *mem,
			  struct dm_report_field *field,
			  const void *data, void *private __attribute__((unused)))
{
	const struct volume_group *vg = (const struct volume_group *) data;
	char *repstr;

	if (!(repstr = vg_attr_dup(mem, vg)))
		return 0;

	dm_report_field_set_value(field, repstr, NULL);
	return 1;
}

static int _segtype_disp(struct dm_report *rh __attribute__((unused)),
			 struct dm_pool *mem __attribute__((unused)),
			 struct dm_report_field *field,
			 const void *data, void *private __attribute__((unused)))
{
	const struct lv_segment *seg = (const struct lv_segment *) data;
	char *name;

	if (!(name = lvseg_segtype_dup(mem, seg))) {
		log_error("Failed to get segtype.");
		return 0;
	}

	dm_report_field_set_value(field, name, NULL);
	return 1;
}

static int _loglv_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
		       struct dm_report_field *field,
		       const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	const char *name;

	if ((name = lv_mirror_log_dup(mem, lv)))
		return dm_report_field_string(rh, field, &name);

	dm_report_field_set_value(field, "", NULL);
	return 1;
}

static int _lvname_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	char *repstr, *lvname;
	size_t len;

	if (lv_is_visible(lv))
		return dm_report_field_string(rh, field, &lv->name);

	len = strlen(lv->name) + 3;
	if (!(repstr = dm_pool_zalloc(mem, len))) {
		log_error("dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, len, "[%s]", lv->name) < 0) {
		log_error("lvname snprintf failed");
		return 0;
	}

	if (!(lvname = dm_pool_strdup(mem, lv->name))) {
		log_error("dm_pool_strdup failed");
		return 0;
	}

	dm_report_field_set_value(field, repstr, lvname);

	return 1;
}

static int _datalv_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
			struct dm_report_field *field,
			const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	const struct lv_segment *seg = lv_is_thin_pool(lv) ? first_seg(lv) : NULL;

	if (seg)
		return _lvname_disp(rh, mem, field, seg_lv(seg, 0), private);

	dm_report_field_set_value(field, "", NULL);
	return 1;
}

static int _metadatalv_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
			    struct dm_report_field *field,
			    const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	const struct lv_segment *seg = lv_is_thin_pool(lv) ? first_seg(lv) : NULL;

	if (seg)
		return _lvname_disp(rh, mem, field, seg->metadata_lv, private);

	dm_report_field_set_value(field, "", NULL);
	return 1;
}

static int _poollv_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
			struct dm_report_field *field,
			const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	struct lv_segment *seg;

	if (lv_is_thin_volume(lv))
		dm_list_iterate_items(seg, &lv->segments)
			if (seg_is_thin_volume(seg))
				return _lvname_disp(rh, mem, field,
						    seg->pool_lv, private);

	dm_report_field_set_value(field, "", NULL);
	return 1;
}

static int _lvpath_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	char *repstr;

	if (!(repstr = lv_path_dup(mem, lv)))
		return 0;

	dm_report_field_set_value(field, repstr, NULL);

	return 1;
}

static int _origin_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;

	if (lv_is_cow(lv))
		return _lvname_disp(rh, mem, field, origin_from_cow(lv), private);

	if (lv_is_thin_volume(lv) && first_seg(lv)->origin)
		return _lvname_disp(rh, mem, field, first_seg(lv)->origin, private);

	if (lv_is_thin_volume(lv) && first_seg(lv)->external_lv)
		return _lvname_disp(rh, mem, field, first_seg(lv)->external_lv, private);

	dm_report_field_set_value(field, "", NULL);
	return 1;
}

static int _movepv_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
			struct dm_report_field *field,
			const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	const char *name;

	if (!(name = lv_move_pv_dup(mem, lv)))
		dm_report_field_set_value(field, "", NULL);
	else
		return dm_report_field_string(rh, field, &name);
	return 1;
}

static int _convertlv_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
			   struct dm_report_field *field,
			   const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	const char *name = NULL;

	name = lv_convert_lv_dup(mem, lv);
	if (name)
		return dm_report_field_string(rh, field, &name);

	dm_report_field_set_value(field, "", NULL);
	return 1;
}

static int _size32_disp(struct dm_report *rh __attribute__((unused)), struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	const uint32_t size = *(const uint32_t *) data;
	const char *disp, *repstr;
	uint64_t *sortval;

	if (!*(disp = display_size_units(private, (uint64_t) size)))
		return_0;

	if (!(repstr = dm_pool_strdup(mem, disp))) {
		log_error("dm_pool_strdup failed");
		return 0;
	}

	if (!(sortval = dm_pool_alloc(mem, sizeof(uint64_t)))) {
		log_error("dm_pool_alloc failed");
		return 0;
	}

	*sortval = (uint64_t) size;

	dm_report_field_set_value(field, repstr, sortval);

	return 1;
}

static int _size64_disp(struct dm_report *rh __attribute__((unused)),
			struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	const uint64_t size = *(const uint64_t *) data;
	const char *disp, *repstr;
	uint64_t *sortval;

	if (!*(disp = display_size_units(private, size)))
		return_0;

	if (!(repstr = dm_pool_strdup(mem, disp))) {
		log_error("dm_pool_strdup failed");
		return 0;
	}

	if (!(sortval = dm_pool_alloc(mem, sizeof(uint64_t)))) {
		log_error("dm_pool_alloc failed");
		return 0;
	}

	*sortval = size;
	dm_report_field_set_value(field, repstr, sortval);

	return 1;
}

static int _uint32_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
			struct dm_report_field *field,
			const void *data, void *private __attribute__((unused)))
{
	return dm_report_field_uint32(rh, field, data);
}

static int _int32_disp(struct dm_report *rh, struct dm_pool *mem __attribute__((unused)),
		       struct dm_report_field *field,
		       const void *data, void *private __attribute__((unused)))
{
	return dm_report_field_int32(rh, field, data);
}

static int _lvreadahead_disp(struct dm_report *rh, struct dm_pool *mem,
			     struct dm_report_field *field,
			     const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;

	if (lv->read_ahead == DM_READ_AHEAD_AUTO) {
		dm_report_field_set_value(field, "auto", &_minusone64);
		return 1;
	}

	return _size32_disp(rh, mem, field, &lv->read_ahead, private);
}

static int _lvkreadahead_disp(struct dm_report *rh, struct dm_pool *mem,
			      struct dm_report_field *field,
			      const void *data,
			      void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	uint32_t read_ahead;

	if ((read_ahead = lv_kernel_read_ahead(lv)) == UINT32_MAX)
		return dm_report_field_int32(rh, field, &_minusone32);

	return _size32_disp(rh, mem, field, &read_ahead, private);
}

static int _vgsize_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	uint64_t size;

	size = (uint64_t) vg_size(vg);

	return _size64_disp(rh, mem, field, &size, private);
}

static int _segmonitor_disp(struct dm_report *rh, struct dm_pool *mem,
			    struct dm_report_field *field,
			    const void *data, void *private)
{
	char *str;

	if (!(str = lvseg_monitor_dup(mem, (const struct lv_segment *)data)))
		return_0;

	dm_report_field_set_value(field, str, NULL);

	return 1;
}

static int _segstart_disp(struct dm_report *rh, struct dm_pool *mem,
			  struct dm_report_field *field,
			  const void *data, void *private)
{
	const struct lv_segment *seg = (const struct lv_segment *) data;
	uint64_t start;

	start = lvseg_start(seg);

	return _size64_disp(rh, mem, field, &start, private);
}

static int _segstartpe_disp(struct dm_report *rh,
			    struct dm_pool *mem __attribute__((unused)),
			    struct dm_report_field *field,
			    const void *data,
			    void *private __attribute__((unused)))
{
	const struct lv_segment *seg = (const struct lv_segment *) data;

	return dm_report_field_uint32(rh, field, &seg->le);
}

static int _segsize_disp(struct dm_report *rh, struct dm_pool *mem,
			 struct dm_report_field *field,
			 const void *data, void *private)
{
	const struct lv_segment *seg = (const struct lv_segment *) data;
	uint64_t size;

	size = lvseg_size(seg);

	return _size64_disp(rh, mem, field, &size, private);
}

static int _chunksize_disp(struct dm_report *rh, struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data, void *private)
{
	const struct lv_segment *seg = (const struct lv_segment *) data;
	uint64_t size;

	size = lvseg_chunksize(seg);

	return _size64_disp(rh, mem, field, &size, private);
}

static int _thinzero_disp(struct dm_report *rh, struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data, void *private)
{
	const struct lv_segment *seg = (const struct lv_segment *) data;

	/* Suppress thin count if not thin pool */
	if (!seg_is_thin_pool(seg)) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	return _uint32_disp(rh, mem, field, &seg->zero_new_blocks, private);
}

static int _transactionid_disp(struct dm_report *rh, struct dm_pool *mem,
				struct dm_report_field *field,
				const void *data, void *private)
{
	const struct lv_segment *seg = (const struct lv_segment *) data;

	/* Suppress thin count if not thin pool */
	if (!seg_is_thin_pool(seg)) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	return  dm_report_field_uint64(rh, field, &seg->transaction_id);
}

static int _discards_disp(struct dm_report *rh, struct dm_pool *mem,
			  struct dm_report_field *field,
			  const void *data, void *private)
{
	const struct lv_segment *seg = (const struct lv_segment *) data;
	const char *discards_str;

	if (seg_is_thin_volume(seg))
		seg = first_seg(seg->pool_lv);

	if (seg_is_thin_pool(seg)) {
		discards_str = get_pool_discards_name(seg->discards);
		return dm_report_field_string(rh, field, &discards_str);
	}

	dm_report_field_set_value(field, "", NULL);

	return 1;
}

static int _originsize_disp(struct dm_report *rh, struct dm_pool *mem,
			    struct dm_report_field *field,
			    const void *data, void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	uint64_t size;

	if (!(size = lv_origin_size(lv))) {
		dm_report_field_set_value(field, "", &_zero64);
		return 1;
	}

	return _size64_disp(rh, mem, field, &size, private);
}

static int _pvused_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;
	uint64_t used;

	used = pv_used(pv);

	return _size64_disp(rh, mem, field, &used, private);
}

static int _pvfree_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;
	uint64_t freespace;

	freespace = pv_free(pv);

	return _size64_disp(rh, mem, field, &freespace, private);
}

static int _pvsize_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;
	uint64_t size;

	size = pv_size_field(pv);

	return _size64_disp(rh, mem, field, &size, private);
}

static int _devsize_disp(struct dm_report *rh, struct dm_pool *mem,
			 struct dm_report_field *field,
			 const void *data, void *private)
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;
	uint64_t size;

	size = pv_dev_size(pv);

	return _size64_disp(rh, mem, field, &size, private);
}

static int _vgfree_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	uint64_t freespace;

	freespace = (uint64_t) vg_free(vg);

	return _size64_disp(rh, mem, field, &freespace, private);
}

static int _uuid_disp(struct dm_report *rh __attribute__((unused)), struct dm_pool *mem,
		      struct dm_report_field *field,
		      const void *data, void *private __attribute__((unused)))
{
	char *repstr = NULL;

	if (!(repstr = id_format_and_copy(mem, data)))
		return_0;

	dm_report_field_set_value(field, repstr, NULL);
	return 1;
}

static int _pvmdas_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	uint32_t count;
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;

	count = pv_mda_count(pv);

	return _uint32_disp(rh, mem, field, &count, private);
}

static int _pvmdasused_disp(struct dm_report *rh, struct dm_pool *mem,
			     struct dm_report_field *field,
			     const void *data, void *private)
{
	uint32_t count;
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;

	count = pv_mda_used_count(pv);

	return _uint32_disp(rh, mem, field, &count, private);
}

static int _vgmdas_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	uint32_t count;

	count = vg_mda_count(vg);

	return _uint32_disp(rh, mem, field, &count, private);
}

static int _vgmdasused_disp(struct dm_report *rh, struct dm_pool *mem,
			     struct dm_report_field *field,
			     const void *data, void *private)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	uint32_t count;

	count = vg_mda_used_count(vg);

	return _uint32_disp(rh, mem, field, &count, private);
}

static int _vgmdacopies_disp(struct dm_report *rh, struct dm_pool *mem,
				   struct dm_report_field *field,
				   const void *data, void *private)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	uint32_t count;

	count = vg_mda_copies(vg);

	if (count == VGMETADATACOPIES_UNMANAGED) {
		dm_report_field_set_value(field, "unmanaged", &_minusone64);
		return 1;
	}

	return _uint32_disp(rh, mem, field, &count, private);
}

static int _vgprofile_disp(struct dm_report *rh, struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data, void *private)
{
	const struct volume_group *vg = (const struct volume_group *) data;

	if (vg->profile)
		return dm_report_field_string(rh, field, &vg->profile->name);

	dm_report_field_set_value(field, "", NULL);
	return 1;
}

static int _pvmdafree_disp(struct dm_report *rh, struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data, void *private)
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;
	uint64_t freespace;

	freespace = pv_mda_free(pv);

	return _size64_disp(rh, mem, field, &freespace, private);
}

static int _pvmdasize_disp(struct dm_report *rh, struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data, void *private)
{
	const struct physical_volume *pv =
	    (const struct physical_volume *) data;
	uint64_t min_mda_size;

	min_mda_size = pv_mda_size(pv);

	return _size64_disp(rh, mem, field, &min_mda_size, private);
}

static int _vgmdasize_disp(struct dm_report *rh, struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data, void *private)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	uint64_t min_mda_size;

	min_mda_size = vg_mda_size(vg);

	return _size64_disp(rh, mem, field, &min_mda_size, private);
}

static int _vgmdafree_disp(struct dm_report *rh, struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data, void *private)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	uint64_t freespace;

	freespace = vg_mda_free(vg);

	return _size64_disp(rh, mem, field, &freespace, private);
}

static int _lvcount_disp(struct dm_report *rh, struct dm_pool *mem,
			 struct dm_report_field *field,
			 const void *data, void *private)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	uint32_t count;

	count = vg_visible_lvs(vg);

	return _uint32_disp(rh, mem, field, &count, private);
}

static int _lvsegcount_disp(struct dm_report *rh, struct dm_pool *mem,
			    struct dm_report_field *field,
			    const void *data, void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	uint32_t count;

	count = dm_list_size(&lv->segments);

	return _uint32_disp(rh, mem, field, &count, private);
}

static int _snapcount_disp(struct dm_report *rh, struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data, void *private)
{
	const struct volume_group *vg = (const struct volume_group *) data;
	uint32_t count;

	count = snapshot_count(vg);

	return _uint32_disp(rh, mem, field, &count, private);
}

static int _snpercent_disp(struct dm_report *rh __attribute__((unused)), struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	percent_t snap_percent;
	uint64_t *sortval;
	char *repstr;

	/* Suppress snapshot percentage if not using driver */
	if (!activation()) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	if (!(sortval = dm_pool_alloc(mem, sizeof(uint64_t)))) {
		log_error("dm_pool_alloc failed");
		return 0;
	}

	if ((!lv_is_cow(lv) && !lv_is_merging_origin(lv)) ||
	    !lv_is_active_locally(lv)) {
		*sortval = UINT64_C(0);
		dm_report_field_set_value(field, "", sortval);
		return 1;
	}

	if (!lv_snapshot_percent(lv, &snap_percent) ||
	    (snap_percent == PERCENT_INVALID) || (snap_percent == PERCENT_MERGE_FAILED)) {
		if (!lv_is_merging_origin(lv)) {
			*sortval = UINT64_C(100);
			dm_report_field_set_value(field, "100.00", sortval);
		} else {
			/* onactivate merge that hasn't started yet would
			 * otherwise display incorrect snap% in origin
			 */
			*sortval = UINT64_C(0);
			dm_report_field_set_value(field, "", sortval);
		}
		return 1;
	}

	if (!(repstr = dm_pool_zalloc(mem, 8))) {
		log_error("dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, 7, "%.2f", percent_to_float(snap_percent)) < 0) {
		log_error("snapshot percentage too large");
		return 0;
	}

	*sortval = (uint64_t)(snap_percent * 1000.f);
	dm_report_field_set_value(field, repstr, sortval);

	return 1;
}

static int _copypercent_disp(struct dm_report *rh __attribute__((unused)),
			     struct dm_pool *mem,
			     struct dm_report_field *field,
			     const void *data, void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	percent_t percent;
	uint64_t *sortval;
	char *repstr;

	if (!(sortval = dm_pool_alloc(mem, sizeof(uint64_t)))) {
		log_error("dm_pool_alloc failed");
		return 0;
	}

	if (lv->status & RAID) {
		if (!lv_raid_percent(lv, &percent) ||
		    (percent == PERCENT_INVALID))
			goto no_copypercent;
	} else if ((!(lv->status & PVMOVE) && !(lv->status & MIRRORED)) ||
		   !lv_mirror_percent(lv->vg->cmd, lv, 0, &percent, NULL) ||
		   (percent == PERCENT_INVALID))
		goto no_copypercent;

	percent = copy_percent(lv);

	if (!(repstr = dm_pool_zalloc(mem, 8))) {
		log_error("dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, 7, "%.2f", percent_to_float(percent)) < 0) {
		log_error("copy percentage too large");
		return 0;
	}

	*sortval = (uint64_t)(percent * 1000.f);
	dm_report_field_set_value(field, repstr, sortval);

	return 1;

no_copypercent:
	*sortval = UINT64_C(0);
	dm_report_field_set_value(field, "", sortval);
	return 1;
}

static int _raidsyncaction_disp(struct dm_report *rh __attribute__((unused)),
			     struct dm_pool *mem,
			     struct dm_report_field *field,
			     const void *data,
			     void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	char *sync_action;

	if (!(lv->status & RAID) ||
	    !lv_raid_sync_action(lv, &sync_action)) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	return _string_disp(rh, mem, field, &sync_action, private);
}

static int _raidmismatchcount_disp(struct dm_report *rh __attribute__((unused)),
				struct dm_pool *mem,
				struct dm_report_field *field,
				const void *data,
				void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	uint64_t mismatch_count;

	if (!(lv->status & RAID) ||
	    !lv_raid_mismatch_count(lv, &mismatch_count)) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	return dm_report_field_uint64(rh, field, &mismatch_count);
}

static int _raidwritebehind_disp(struct dm_report *rh __attribute__((unused)),
			      struct dm_pool *mem,
			      struct dm_report_field *field,
			      const void *data,
			      void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;

	if (!lv_is_raid_type(lv) || !first_seg(lv)->writebehind) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	return dm_report_field_uint32(rh, field, &first_seg(lv)->writebehind);
}

static int _raidminrecoveryrate_disp(struct dm_report *rh __attribute__((unused)),
				   struct dm_pool *mem,
				   struct dm_report_field *field,
				   const void *data,
				   void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;

	if (!lv_is_raid_type(lv) || !first_seg(lv)->min_recovery_rate) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	return dm_report_field_uint32(rh, field,
				      &first_seg(lv)->min_recovery_rate);
}

static int _raidmaxrecoveryrate_disp(struct dm_report *rh __attribute__((unused)),
				   struct dm_pool *mem,
				   struct dm_report_field *field,
				   const void *data,
				   void *private __attribute__((unused)))
{
	const struct logical_volume *lv = (const struct logical_volume *) data;

	if (!lv_is_raid_type(lv) || !first_seg(lv)->max_recovery_rate) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	return dm_report_field_uint32(rh, field,
				      &first_seg(lv)->max_recovery_rate);
}

static int _dtpercent_disp(int metadata, struct dm_report *rh,
			   struct dm_pool *mem,
			   struct dm_report_field *field,
			   const void *data, void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	struct lvinfo info;
	percent_t percent;
	uint64_t *sortval;
	char *repstr;

	/* Suppress data percent if not thin pool/volume or not using driver */
	if (!lv_info(lv->vg->cmd, lv, 1, &info, 0, 0) || !info.exists) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	if (!(sortval = dm_pool_zalloc(mem, sizeof(uint64_t)))) {
		log_error("Failed to allocate sortval.");
		return 0;
	}

	if (lv_is_thin_pool(lv)) {
		if (!lv_thin_pool_percent(lv, metadata, &percent))
			return_0;
	} else { /* thin_volume */
		if (!lv_thin_percent(lv, 0, &percent))
			return_0;
	}

	if (!(repstr = dm_pool_alloc(mem, 8))) {
		log_error("Failed to allocate report buffer.");
		return 0;
	}

	if (dm_snprintf(repstr, 8, "%.2f", percent_to_float(percent)) < 0) {
		log_error("Data percentage too large.");
		return 0;
	}

	*sortval = (uint64_t)(percent * 1000.f);
	dm_report_field_set_value(field, repstr, sortval);

	return 1;
}

static int _datapercent_disp(struct dm_report *rh, struct dm_pool *mem,
			     struct dm_report_field *field,
			     const void *data, void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;

	if (lv_is_cow(lv))
		return _snpercent_disp(rh, mem, field, data, private);

	if (lv_is_thin_pool(lv) || lv_is_thin_volume(lv))
		return _dtpercent_disp(0, rh, mem, field, data, private);

	dm_report_field_set_value(field, "", NULL);

	return 1;
}

static int _metadatapercent_disp(struct dm_report *rh, struct dm_pool *mem,
				 struct dm_report_field *field,
				 const void *data, void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;

	if (lv_is_thin_pool(lv))
		return _dtpercent_disp(1, rh, mem, field, data, private);

	dm_report_field_set_value(field, "", NULL);

	return 1;
}

static int _lvmetadatasize_disp(struct dm_report *rh, struct dm_pool *mem,
				struct dm_report_field *field,
				const void *data, void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	uint64_t size;

	if (!lv_is_thin_pool(lv)) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	size = lv_metadata_size(lv);

	return _size64_disp(rh, mem, field, &size, private);
}

static int _thincount_disp(struct dm_report *rh, struct dm_pool *mem,
                         struct dm_report_field *field,
                         const void *data, void *private)
{
	const struct lv_segment *seg = (const struct lv_segment *) data;
	uint32_t count;

	/* Suppress thin count if not thin pool */
	if (!seg_is_thin_pool(seg)) {
		dm_report_field_set_value(field, "", NULL);
		return 1;
	}

	count = dm_list_size(&seg->lv->segs_using_this_lv);

	return _uint32_disp(rh, mem, field, &count, private);
}

static int _lvtime_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	char *repstr;
	uint64_t *sortval;

	if (!(sortval = dm_pool_zalloc(mem, sizeof(uint64_t)))) {
		log_error("Failed to allocate sortval.");
		return 0;
	}

	*sortval = lv->timestamp;
	if (!(repstr = lv_time_dup(mem, lv)))
		return_0;

	dm_report_field_set_value(field, repstr, sortval);

	return 1;
}

static int _lvhost_disp(struct dm_report *rh, struct dm_pool *mem,
			struct dm_report_field *field,
			const void *data, void *private)
{
	const struct logical_volume *lv = (const struct logical_volume *) data;
	char *repstr;

	if (!(repstr = lv_host_dup(mem, lv)))
		return_0;

	dm_report_field_set_value(field, repstr, repstr);

	return 1;
}

static int _lvactive_disp(struct dm_report *rh, struct dm_pool *mem,
			     struct dm_report_field *field,
			     const void *data, void *private)
{
	char *repstr;

	if (!(repstr = lv_active_dup(mem, (const struct logical_volume *) data)))
		return_0;

	dm_report_field_set_value(field, repstr, NULL);

	return 1;
}

/* Report object types */

/* necessary for displaying something for PVs not belonging to VG */
static struct format_instance _dummy_fid = {
	.metadata_areas_in_use = { &(_dummy_fid.metadata_areas_in_use), &(_dummy_fid.metadata_areas_in_use) },
	.metadata_areas_ignored = { &(_dummy_fid.metadata_areas_ignored), &(_dummy_fid.metadata_areas_ignored) },
};

static struct volume_group _dummy_vg = {
	.fid = &_dummy_fid,
	.name = "",
	.system_id = (char *) "",
	.pvs = { &(_dummy_vg.pvs), &(_dummy_vg.pvs) },
	.lvs = { &(_dummy_vg.lvs), &(_dummy_vg.lvs) },
	.tags = { &(_dummy_vg.tags), &(_dummy_vg.tags) },
};

static void *_obj_get_vg(void *obj)
{
	struct volume_group *vg = ((struct lvm_report_object *)obj)->vg;

	return vg ? vg : &_dummy_vg;
}

static void *_obj_get_lv(void *obj)
{
	return ((struct lvm_report_object *)obj)->lv;
}

static void *_obj_get_pv(void *obj)
{
	return ((struct lvm_report_object *)obj)->pv;
}

static void *_obj_get_seg(void *obj)
{
	return ((struct lvm_report_object *)obj)->seg;
}

static void *_obj_get_pvseg(void *obj)
{
	return ((struct lvm_report_object *)obj)->pvseg;
}

static const struct dm_report_object_type _report_types[] = {
	{ VGS, "Volume Group", "vg_", _obj_get_vg },
	{ LVS, "Logical Volume", "lv_", _obj_get_lv },
	{ PVS, "Physical Volume", "pv_", _obj_get_pv },
	{ LABEL, "Physical Volume Label", "pv_", _obj_get_pv },
	{ SEGS, "Logical Volume Segment", "seg_", _obj_get_seg },
	{ PVSEGS, "Physical Volume Segment", "pvseg_", _obj_get_pvseg },
	{ 0, "", "", NULL },
};

/*
 * Import column definitions
 */

#define STR DM_REPORT_FIELD_TYPE_STRING
#define NUM DM_REPORT_FIELD_TYPE_NUMBER
#define FIELD(type, strct, sorttype, head, field, width, func, id, desc, writeable) \
	{type, sorttype, offsetof(type_ ## strct, field), width, \
	 #id, head, &_ ## func ## _disp, desc},

typedef struct physical_volume type_pv;
typedef struct logical_volume type_lv;
typedef struct volume_group type_vg;
typedef struct lv_segment type_seg;
typedef struct pv_segment type_pvseg;

static const struct dm_report_field_type _fields[] = {
#include "columns.h"
{0, 0, 0, 0, "", "", NULL, NULL},
};

#undef STR
#undef NUM
#undef FIELD

void *report_init(struct cmd_context *cmd, const char *format, const char *keys,
		  report_type_t *report_type, const char *separator,
		  int aligned, int buffered, int headings, int field_prefixes,
		  int quoted, int columns_as_rows)
{
	uint32_t report_flags = 0;
	void *rh;

	if (aligned)
		report_flags |= DM_REPORT_OUTPUT_ALIGNED;

	if (buffered)
		report_flags |= DM_REPORT_OUTPUT_BUFFERED;

	if (headings)
		report_flags |= DM_REPORT_OUTPUT_HEADINGS;

	if (field_prefixes)
		report_flags |= DM_REPORT_OUTPUT_FIELD_NAME_PREFIX;

	if (!quoted)
		report_flags |= DM_REPORT_OUTPUT_FIELD_UNQUOTED;

	if (columns_as_rows)
		report_flags |= DM_REPORT_OUTPUT_COLUMNS_AS_ROWS;

	rh = dm_report_init(report_type, _report_types, _fields, format,
			    separator, report_flags, keys, cmd);

	if (rh && field_prefixes)
		dm_report_set_output_field_name_prefix(rh, "lvm2_");

	return rh;
}

/*
 * Create a row of data for an object
 */
int report_object(void *handle, struct volume_group *vg,
		  struct logical_volume *lv, struct physical_volume *pv,
		  struct lv_segment *seg, struct pv_segment *pvseg)
{
	struct lvm_report_object obj;

	/* The two format fields might as well match. */
	if (!vg && pv)
		_dummy_fid.fmt = pv->fmt;

	obj.vg = vg;
	obj.lv = lv;
	obj.pv = pv;
	obj.seg = seg;
	obj.pvseg = pvseg;

	return dm_report_object(handle, &obj);
}
