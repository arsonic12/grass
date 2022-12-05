/*
 * r.univar
 *
 *  Calculates univariate statistics from the non-null cells of a GRASS raster
 *  map
 *
 *   Copyright (C) 2004-2006, 2012 by the GRASS Development Team
 *   Author(s): Hamish Bowman, University of Otago, New Zealand
 *              Extended stats: Martin Landa
 *              Zonal stats: Markus Metz
 *
 *      This program is free software under the GNU General Public
 *      License (>=v2). Read the file COPYING that comes with GRASS
 *      for details.
 *
 *   This program is a replacement for the r.univar shell script
 */

#if defined(_OPENMP)
#include <omp.h>
#endif

#include <assert.h>
#include <string.h>
#include <float.h>
#include "globals.h"

param_type param;
zone_type zone_info;
int nprocs;

/* ************************************************************************* */
/* Set up the arguments we are expecting ********************************** */
/* ************************************************************************* */
void set_params(void)
{
    param.inputfile = G_define_standard_option(G_OPT_R_MAPS);

    param.zonefile = G_define_standard_option(G_OPT_R_MAP);
    param.zonefile->key = "zones";
    param.zonefile->required = NO;
    param.zonefile->description =
        _("Raster map used for zoning, must be of type CELL");

    param.output_file = G_define_standard_option(G_OPT_F_OUTPUT);
    param.output_file->required = NO;
    param.output_file->description =
        _("Name for output file (if omitted or \"-\" output to stdout)");
    param.output_file->guisection = _("Output settings");

    param.percentile = G_define_option();
    param.percentile->key = "percentile";
    param.percentile->type = TYPE_DOUBLE;
    param.percentile->required = NO;
    param.percentile->multiple = YES;
    param.percentile->options = "0-100";
    param.percentile->answer = "90";
    param.percentile->description =
        _("Percentile to calculate (requires extended statistics flag)");
    param.percentile->guisection = _("Extended");

    param.nprocs = G_define_standard_option(G_OPT_M_NPROCS);

    param.separator = G_define_standard_option(G_OPT_F_SEP);
    param.separator->guisection = _("Formatting");

    param.shell_style = G_define_flag();
    param.shell_style->key = 'g';
    param.shell_style->description = _("Print the stats in shell script style");
    param.shell_style->guisection = _("Formatting");

    param.extended = G_define_flag();
    param.extended->key = 'e';
    param.extended->description = _("Calculate extended statistics");
    param.extended->guisection = _("Extended");

    param.table = G_define_flag();
    param.table->key = 't';
    param.table->description =
        _("Table output format instead of standard output format");
    param.table->guisection = _("Formatting");

    param.use_rast_region = G_define_flag();
    param.use_rast_region->key = 'r';
    param.use_rast_region->description =
        _("Use the native resolution and extent of the raster map, instead of "
          "the current region");

    return;
}

static int open_raster(const char *infile);
static univar_stat *univar_stat_with_percentiles(int map_type);
static void process_raster(univar_stat *stats, int *fd, int *fdz,
                           const struct Cell_head *region);

/* *************************************************************** */
/* **** the main functions for r.univar ************************** */
/* *************************************************************** */
int main(int argc, char *argv[])
{
    int rasters;

    struct Cell_head region;
    struct GModule *module;
    univar_stat *stats;
    char **p, *z;
    int *fd, *fdz, cell_type, min, max;
    struct Range zone_range;
    const char *mapset, *name;
    int t;

    G_gisinit(argv[0]);

    module = G_define_module();
    G_add_keyword(_("raster"));
    G_add_keyword(_("statistics"));
    G_add_keyword(_("univariate statistics"));
    G_add_keyword(_("zonal statistics"));
    G_add_keyword(_("parallel"));

    module->label = _("Calculates univariate statistics from the non-null "
                      "cells of a raster map.");
    module->description =
        _("Statistics include number of cells counted, minimum and maximum cell"
          " values, range, arithmetic mean, population variance, standard "
          "deviation,"
          " coefficient of variation, and sum.");

    /* Define the different options */
    set_params();

    if (G_parser(argc, argv))
        exit(EXIT_FAILURE);

    if (param.zonefile->answer && param.use_rast_region->answer) {
        G_fatal_error(
            _("zones option and region flag -r are mutually exclusive"));
    }

    name = param.output_file->answer;
    if (name != NULL && strcmp(name, "-") != 0) {
        if (NULL == freopen(name, "w", stdout)) {
            G_fatal_error(_("Unable to open file <%s> for writing"), name);
        }
    }

    /* set nprocs parameter */
    sscanf(param.nprocs->answer, "%d", &nprocs);
    if (nprocs < 1)
        G_fatal_error(_("<%d> is not valid number of nprocs."), nprocs);
#if defined(_OPENMP)
    if (param.extended->answer) {
        /* Calculation of extended statistics is not parallelized yet */
        if (nprocs > 1)
            G_warning(_("Computing extending statistics is not parallelized "
                        "yet. Ignoring threads setting."));
        nprocs = 1;
    }
    else {
        omp_set_num_threads(nprocs);
    }
#else
    if (nprocs != 1)
        G_warning(_("GRASS is compiled without OpenMP support. Ignoring "
                    "threads setting."));
    nprocs = 1;
#endif

    /* table field separator */
    zone_info.sep = G_option_to_separator(param.separator);

    zone_info.min = 0;
    zone_info.max = 0;
    zone_info.n_zones = 0;

    fdz = NULL;
    fd = G_malloc(nprocs * sizeof(int));

    /* open zoning raster */
    if ((z = param.zonefile->answer)) {
        mapset = G_find_raster2(z, "");

        fdz = G_malloc(nprocs * sizeof(int));
        for (t = 0; t < nprocs; t++)
            fdz[t] = open_raster(z);

        cell_type = Rast_get_map_type(fdz[0]);
        if (cell_type != CELL_TYPE)
            G_fatal_error("Zoning raster must be of type CELL");

        if (Rast_read_range(z, mapset, &zone_range) == -1)
            G_fatal_error("Can not read range for zoning raster");
        Rast_get_range_min_max(&zone_range, &min, &max);
        if (Rast_read_cats(z, mapset, &(zone_info.cats)))
            G_warning("no category support for zoning raster");

        zone_info.min = min;
        zone_info.max = max;
        zone_info.n_zones = max - min + 1;
    }

    /* count the input rasters given */
    for (p = (char **)param.inputfile->answers, rasters = 0; *p; p++, rasters++)
        ;

    /* process all input rasters */
    int map_type = param.extended->answer ? -2 : -1;

    stats = ((map_type == -1) ? create_univar_stat_struct(-1, 0) : 0);

    for (p = param.inputfile->answers; *p; p++) {

        /* Check if the native extent and resolution
           of the input map should be used */
        if (param.use_rast_region->answer) {
            mapset = G_find_raster2(*p, "");
            Rast_get_cellhd(*p, mapset, &region);
            /* Set the computational region */
            Rast_set_window(&region);
        }
        else {
            G_get_window(&region);
        }

        for (t = 0; t < nprocs; t++)
            fd[t] = open_raster(*p);

        if (map_type != -1) {
            /* NB: map_type must match when doing extended stats */
            int this_type = Rast_get_map_type(fd[0]);

            assert(this_type > -1);
            if (map_type < -1) {
                /* extended stats */
                assert(stats == 0);
                map_type = this_type;
                stats = univar_stat_with_percentiles(map_type);
            }
            else if (this_type != map_type) {
                G_fatal_error(_("Raster <%s> type mismatch"), *p);
            }
        }

        process_raster(stats, fd, fdz, &region);

        /* close input raster */
        for (t = 0; t < nprocs; t++)
            Rast_close(fd[t]);
    }

    /* close zoning raster */
    if (z) {
        for (t = 0; t < nprocs; t++)
            Rast_close(fdz[t]);
    }

    /* create the output */
    if (param.table->answer)
        print_stats_table(stats);
    else
        print_stats(stats);

    /* release memory */
    free_univar_stat_struct(stats);

    exit(EXIT_SUCCESS);
}

static int open_raster(const char *infile)
{
    const char *mapset;
    int fd;

    mapset = G_find_raster2(infile, "");
    if (mapset == NULL) {
        G_fatal_error(_("Raster map <%s> not found"), infile);
    }

    fd = Rast_open_old(infile, mapset);
    G_free((void *)mapset);

    return fd;
}

static univar_stat *univar_stat_with_percentiles(int map_type)
{
    univar_stat *stats;
    unsigned int i, j;
    unsigned int n_zones = zone_info.n_zones;

    if (n_zones == 0)
        n_zones = 1;

    i = 0;
    while (param.percentile->answers[i])
        i++;
    stats = create_univar_stat_struct(map_type, i);
    for (i = 0; i < n_zones; i++) {
        for (j = 0; j < stats[i].n_perc; j++) {
            sscanf(param.percentile->answers[j], "%lf", &(stats[i].perc[j]));
        }
    }

    /* . */
    return stats;
}

static void process_raster(univar_stat *stats, int *fd, int *fdz,
                           const struct Cell_head *region)
{
    /* use G_window_rows(), G_window_cols() here? */
    const unsigned int rows = region->rows;
    const unsigned int cols = region->cols;

    const RASTER_MAP_TYPE map_type = Rast_get_map_type(fd[0]);
    const size_t value_sz = Rast_cell_size(map_type);
    int t;
    int z;
    unsigned int row;
    void **raster_row;
    CELL **zoneraster_row = NULL;
    int n_zones = zone_info.n_zones;
    int n_alloc = n_zones ? n_zones : 1;

    raster_row = G_malloc(nprocs * sizeof(void *));
    if (n_zones) {
        zoneraster_row = G_malloc(nprocs * sizeof(CELL *));
    }
    for (t = 0; t < nprocs; t++) {
        raster_row[t] = Rast_allocate_buf(map_type);
        if (n_zones) {
            zoneraster_row[t] = Rast_allocate_c_buf();
        }
    }

#if defined(_OPENMP)
    omp_lock_t *minmax = G_malloc(n_alloc * sizeof(omp_lock_t));

    for (z = 0; z < n_alloc; z++) {
        omp_init_lock(&minmax[z]);
    }
#endif

    int computed = 0;

#pragma omp parallel if (nprocs > 1)
    {
        int z;
        int t_id = 0;

#if defined(_OPENMP)
        if (!param.extended->answer) {
            t_id = omp_get_thread_num();
        }
#endif
        size_t *n = G_calloc(n_alloc, sizeof(size_t));
        double *sum = G_calloc(n_alloc, sizeof(double));
        double *sumsq = G_calloc(n_alloc, sizeof(double));
        double *sum_abs = G_calloc(n_alloc, sizeof(double));
        size_t *size = G_calloc(n_alloc, sizeof(size_t));
        double *min = G_malloc(n_alloc * sizeof(double));
        double *max = G_malloc(n_alloc * sizeof(double));

        bucket *buckets = G_malloc(n_alloc * sizeof(bucket));

        for (z = 0; z < n_alloc; z++) {
            max[z] = DBL_MIN;
            min[z] = DBL_MAX;
            buckets[z].n = 0;
            buckets[z].n_alloc = 0;
            buckets[z].nextp = NULL;
            buckets[z].cells = NULL;
            buckets[z].fcells = NULL;
            buckets[z].dcells = NULL;
        }

#pragma omp for schedule(static)
        for (row = 0; row < rows; row++) {
            void *ptr;
            CELL *zptr = NULL;
            unsigned int col;

            Rast_get_row(fd[t_id], raster_row[t_id], row, map_type);
            if (n_zones) {
                Rast_get_c_row(fdz[t_id], zoneraster_row[t_id], row);
                zptr = zoneraster_row[t_id];
            }

            ptr = raster_row[t_id];

            for (col = 0; col < cols; col++) {
                double val;
                int zone = 0;

                if (n_zones) {
                    /* skip NULL cells in zone map */
                    if (Rast_is_c_null_value(zptr)) {
                        ptr = G_incr_void_ptr(ptr, value_sz);
                        zptr++;
                        continue;
                    }
                    zone = *zptr - zone_info.min;
                }

                /* count all including NULL cells in input map */
                size[zone]++;

                /* can't do stats with NULL cells in input map */
                if (Rast_is_null_value(ptr, map_type)) {
                    ptr = G_incr_void_ptr(ptr, value_sz);
                    if (n_zones)
                        zptr++;
                    continue;
                }

                if (param.extended->answer) {
                    bucket *t_bkt = &buckets[zone];

                    /* check allocated memory */
                    if (t_bkt->n >= t_bkt->n_alloc) {
                        t_bkt->n_alloc += 1000;
                        size_t msize;

                        switch (map_type) {
                        case DCELL_TYPE:
                            msize = t_bkt->n_alloc * sizeof(DCELL);
                            t_bkt->dcells = (DCELL *)G_realloc(
                                (void *)t_bkt->dcells, msize);
                            t_bkt->nextp = (void *)&(t_bkt->dcells[t_bkt->n]);
                            break;
                        case FCELL_TYPE:
                            msize = t_bkt->n_alloc * sizeof(FCELL);
                            t_bkt->fcells = (FCELL *)G_realloc(
                                (void *)t_bkt->fcells, msize);
                            t_bkt->nextp = (void *)&(t_bkt->fcells[t_bkt->n]);
                            break;
                        case CELL_TYPE:
                            msize = t_bkt->n_alloc * sizeof(CELL);
                            t_bkt->cells =
                                (CELL *)G_realloc((void *)t_bkt->cells, msize);
                            t_bkt->nextp = (void *)&(t_bkt->cells[t_bkt->n]);
                            break;
                        }
                    }
                    /* put the value into stats->XXXcell_array */
                    memcpy(t_bkt->nextp, ptr, value_sz);
                    t_bkt->nextp = G_incr_void_ptr(t_bkt->nextp, value_sz);
                }

                val = ((map_type == DCELL_TYPE)   ? *((DCELL *)ptr)
                       : (map_type == FCELL_TYPE) ? *((FCELL *)ptr)
                                                  : *((CELL *)ptr));

                sum[zone] += val;
                sumsq[zone] += val * val;
                sum_abs[zone] += fabs(val);

                if (val > max[zone])
                    max[zone] = val;
                if (val < min[zone])
                    min[zone] = val;

                ptr = G_incr_void_ptr(ptr, value_sz);
                if (n_zones)
                    zptr++;
                buckets[zone].n++;
            } /* end column loop */
            if (!(param.shell_style->answer)) {
#pragma omp atomic update
                computed++;
                G_percent(computed, rows, 2);
            }
        } /* end row loop */

        for (z = 0; z < n_alloc; z++) {
            if (param.extended->answer) {
#pragma omp critical
                {
                    /*
                       Transfers for each thread from local bucket to global
                       buffer. Case 1: first transfer, skip reallocation and
                       point to the buffer. Case 2: other transfers, reallocate
                       exactly if there is any non-empty bucket.
                     */
                    bucket *t_bucket = &buckets[z];
                    univar_stat *g_buffer = &stats[z];
                    size_t old_size;
                    size_t add_size;

                    switch (map_type) {
                    case DCELL_TYPE:
                        if (NULL == g_buffer->dcell_array) {
                            g_buffer->dcell_array = t_bucket->dcells;
                        }
                        else if (t_bucket->n != 0) {
                            old_size = g_buffer->n * sizeof(DCELL);
                            add_size = t_bucket->n * sizeof(DCELL);

                            g_buffer->dcell_array = (DCELL *)G_realloc(
                                (void *)g_buffer->dcell_array,
                                old_size + add_size);
                            memcpy(&g_buffer->dcell_array[g_buffer->n],
                                   t_bucket->dcells, add_size);
                        }
                        break;
                    case FCELL_TYPE:
                        if (NULL == g_buffer->fcell_array) {
                            g_buffer->fcell_array = t_bucket->fcells;
                        }
                        else if (t_bucket->n != 0) {
                            old_size = g_buffer->n * sizeof(FCELL);
                            add_size = t_bucket->n * sizeof(FCELL);

                            g_buffer->fcell_array = (FCELL *)G_realloc(
                                (void *)g_buffer->fcell_array,
                                old_size + add_size);
                            memcpy(&g_buffer->fcell_array[g_buffer->n],
                                   t_bucket->fcells, add_size);
                        }
                        break;
                    case CELL_TYPE:
                        if (NULL == g_buffer->cell_array) {
                            g_buffer->cell_array = t_bucket->cells;
                        }
                        else if (t_bucket->n != 0) {
                            old_size = g_buffer->n * sizeof(CELL);
                            add_size = t_bucket->n * sizeof(CELL);

                            g_buffer->cell_array =
                                (CELL *)G_realloc((void *)g_buffer->cell_array,
                                                  old_size + add_size);
                            memcpy(&g_buffer->cell_array[g_buffer->n],
                                   t_bucket->cells, add_size);
                        }
                        break;
                    }

                    g_buffer->n += t_bucket->n;
                }
            }
            else {
#pragma omp atomic update
                stats[z].n += buckets[z].n;
            }
#pragma omp atomic update
            stats[z].size += size[z];
#pragma omp atomic update
            stats[z].sum += sum[z];
#pragma omp atomic update
            stats[z].sumsq += sumsq[z];
#pragma omp atomic update
            stats[z].sum_abs += sum_abs[z];

#if defined(_OPENMP)
            omp_set_lock(&minmax[z]);
#endif
            if (stats[z].max < max[z] ||
                (stats[z].max != stats[z].max && max[z] != DBL_MIN)) {
                stats[z].max = max[z];
            }
            if (stats[z].min > min[z] ||
                (stats[z].min != stats[z].min && min[z] != DBL_MAX)) {
                stats[z].min = min[z];
            }
#if defined(_OPENMP)
            omp_unset_lock(&minmax[z]);
#endif
        }

        /* Free per-thread variables */
        for (z = 0; z < n_alloc; z++) {
            if (buckets[z].nextp) {
                G_free(buckets[z].nextp);
            }
            if (buckets[z].cells) {
                G_free(buckets[z].cells);
            }
            if (buckets[z].fcells) {
                G_free(buckets[z].fcells);
            }
            if (buckets[z].dcells) {
                G_free(buckets[z].fcells);
            }
        }
        G_free(buckets);

        G_free(n);
        G_free(sum);
        G_free(sumsq);
        G_free(sum_abs);
        G_free(size);
        G_free(min);
        G_free(max);
    } /* end parallel region */

#if defined(_OPENMP)
    for (z = 0; z < n_alloc; z++) {
        omp_destroy_lock(&minmax[z]);
    }

    G_free(minmax);
#endif

    for (t = 0; t < nprocs; t++) {
        G_free(raster_row[t]);
    }
    G_free(raster_row);
    if (n_zones) {
        for (t = 0; t < nprocs; t++) {
            G_free(zoneraster_row[t]);
        }
        G_free(zoneraster_row);
    }
    if (!(param.shell_style->answer))
        G_percent(rows, rows, 2);
}
