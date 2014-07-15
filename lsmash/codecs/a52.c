/*****************************************************************************
 * a52.c:
 *****************************************************************************
 * Copyright (C) 2012-2014 L-SMASH project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license. */

#include "common/internal.h" /* must be placed first */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "core/box.h"

static const char *bit_stream_mode[] =
    {
        "Main audio service: complete main (CM)",
        "Main audio service: music and effects (ME)",
        "Associated service: visually impaired (VI)",
        "Associated service: hearing impaired (HI)",
        "Associated service: dialogue (D)",
        "Associated service: commentary (C)",
        "Associated service: emergency (E)",
        "Undefined service",
        "Associated service: voice over (VO)",  /* only if acmod == 0b001 */
        "Main audio service: karaoke"
    };

/* For karaoke mode, C->M, S->V1, SL->V1 and SR->V2. */
static const char *audio_coding_mode[] =
    {
        "1 + 1: Dual mono",
        "1/0: C",
        "2/0: L, R",
        "3/0: L, C, R",
        "2/1: L, R, S",
        "3/1: L, C, R, S",
        "2/2: L, R, SL, SR",
        "3/2: L, C, R, SL, SR",
        "Undefined audio coding mode",
        "Undefined audio coding mode",
        "2/0: L, R",
        "3/0: L, M, R",
        "2/1: L, R, V1",
        "3/1: L, M, R, V1",
        "2/2: L, R, V1, V2",
        "3/2: L, M, R, V1, V2"
    };

/***************************************************************************
    AC-3 tools
    ETSI TS 102 366 V1.2.1 (2008-08)
***************************************************************************/
#include "a52.h"

#define AC3_SPECIFIC_BOX_LENGTH 11

uint8_t *lsmash_create_ac3_specific_info( lsmash_ac3_specific_parameters_t *param, uint32_t *data_length )
{
    lsmash_bits_t bits = { 0 };
    lsmash_bs_t   bs   = { 0 };
    lsmash_bits_init( &bits, &bs );
    uint8_t buffer[AC3_SPECIFIC_BOX_LENGTH] = { 0 };
    bs.buffer.data  = buffer;
    bs.buffer.alloc = AC3_SPECIFIC_BOX_LENGTH;
    lsmash_bits_put( &bits, 32, AC3_SPECIFIC_BOX_LENGTH );      /* box size */
    lsmash_bits_put( &bits, 32, ISOM_BOX_TYPE_DAC3.fourcc );    /* box type: 'dac3' */
    lsmash_bits_put( &bits, 2, param->fscod );
    lsmash_bits_put( &bits, 5, param->bsid );
    lsmash_bits_put( &bits, 3, param->bsmod );
    lsmash_bits_put( &bits, 3, param->acmod );
    lsmash_bits_put( &bits, 1, param->lfeon );
    lsmash_bits_put( &bits, 5, param->frmsizecod >> 1 );
    lsmash_bits_put( &bits, 5, 0 );
    uint8_t *data = lsmash_bits_export_data( &bits, data_length );
    lsmash_bits_empty( &bits );
    return data;
}

int lsmash_setup_ac3_specific_parameters_from_syncframe( lsmash_ac3_specific_parameters_t *param, uint8_t *data, uint32_t data_length )
{
    if( !data || data_length < AC3_MIN_SYNCFRAME_LENGTH )
        return -1;
    IF_A52_SYNCWORD( data )
        return -1;
    lsmash_bits_t bits = { 0 };
    lsmash_bs_t   bs   = { 0 };
    uint8_t buffer[AC3_MAX_SYNCFRAME_LENGTH] = { 0 };
    bs.buffer.data  = buffer;
    bs.buffer.alloc = AC3_MAX_SYNCFRAME_LENGTH;
    ac3_info_t  handler = { { 0 } };
    ac3_info_t *info    = &handler;
    memcpy( info->buffer, data, LSMASH_MIN( data_length, AC3_MAX_SYNCFRAME_LENGTH ) );
    info->bits = &bits;
    lsmash_bits_init( &bits, &bs );
    if( ac3_parse_syncframe_header( info, info->buffer ) )
        return -1;
    *param = info->dac3_param;
    return 0;
}

static int ac3_check_syncframe_header( lsmash_ac3_specific_parameters_t *param )
{
    if( param->fscod == 0x3 )
        return -1;      /* unknown Sample Rate Code */
    if( param->frmsizecod > 0x25 )
        return -1;      /* unknown Frame Size Code */
    if( param->bsid >= 10 )
        return -1;      /* might be EAC-3 */
    return 0;
}

int ac3_parse_syncframe_header( ac3_info_t *info, uint8_t *data )
{
    lsmash_bits_t *bits = info->bits;
    if( lsmash_bits_import_data( bits, data, AC3_MIN_SYNCFRAME_LENGTH ) )
        return -1;
    lsmash_ac3_specific_parameters_t *param = &info->dac3_param;
    lsmash_bits_get( bits, 32 );        /* syncword + crc1 */
    param->fscod      = lsmash_bits_get( bits, 2 );
    param->frmsizecod = lsmash_bits_get( bits, 6 );
    param->bsid       = lsmash_bits_get( bits, 5 );
    param->bsmod      = lsmash_bits_get( bits, 3 );
    param->acmod      = lsmash_bits_get( bits, 3 );
    if( (param->acmod & 0x01) && (param->acmod != 0x01) )
        lsmash_bits_get( bits, 2 );     /* cmixlev */
    if( param->acmod & 0x04 )
        lsmash_bits_get( bits, 2 );     /* surmixlev */
    if( param->acmod == 0x02 )
        lsmash_bits_get( bits, 2 );     /* dsurmod */
    param->lfeon = lsmash_bits_get( bits, 1 );
    lsmash_bits_empty( bits );
    return ac3_check_syncframe_header( param );
}

int ac3_construct_specific_parameters( lsmash_codec_specific_t *dst, lsmash_codec_specific_t *src )
{
    assert( dst && dst->data.structured && src && src->data.unstructured );
    if( src->size < AC3_SPECIFIC_BOX_LENGTH )
        return -1;
    lsmash_ac3_specific_parameters_t *param = (lsmash_ac3_specific_parameters_t *)dst->data.structured;
    uint8_t *data = src->data.unstructured;
    uint64_t size = LSMASH_GET_BE32( data );
    data += ISOM_BASEBOX_COMMON_SIZE;
    if( size == 1 )
    {
        size = LSMASH_GET_BE64( data );
        data += 8;
    }
    if( size != src->size )
        return -1;
    param->fscod      = (data[0] >> 6) & 0x03;                                  /* XXxx xxxx xxxx xxxx xxxx xxxx */
    param->bsid       = (data[0] >> 1) & 0x1F;                                  /* xxXX XXXx xxxx xxxx xxxx xxxx */
    param->bsmod      = ((data[0] & 0x01) << 2) | ((data[2] >> 6) & 0x03);      /* xxxx xxxX XXxx xxxx xxxx xxxx */
    param->acmod      = (data[1] >> 3) & 0x07;                                  /* xxxx xxxx xxXX Xxxx xxxx xxxx */
    param->lfeon      = (data[1] >> 2) & 0x01;                                  /* xxxx xxxx xxxx xXxx xxxx xxxx */
    param->frmsizecod = ((data[1] & 0x03) << 3) | ((data[3] >> 5) & 0x07);      /* xxxx xxxx xxxx xxXX XXXx xxxx */
    param->frmsizecod <<= 1;
    return 0;
}

int ac3_print_codec_specific( FILE *fp, lsmash_file_t *file, isom_box_t *box, int level )
{
    assert( fp && file && box && (box->manager & LSMASH_BINARY_CODED_BOX) );
    int indent = level;
    lsmash_ifprintf( fp, indent++, "[%s: AC3 Specific Box]\n", isom_4cc2str( box->type.fourcc ) );
    lsmash_ifprintf( fp, indent, "position = %"PRIu64"\n", box->pos );
    lsmash_ifprintf( fp, indent, "size = %"PRIu64"\n", box->size );
    if( box->size < AC3_SPECIFIC_BOX_LENGTH )
        return -1;
    uint8_t *data = box->binary;
    isom_skip_box_common( &data );
    uint8_t fscod         = (data[0] >> 6) & 0x03;
    uint8_t bsid          = (data[0] >> 1) & 0x1F;
    uint8_t bsmod         = ((data[0] & 0x01) << 2) | ((data[1] >> 6) & 0x03);
    uint8_t acmod         = (data[1] >> 3) & 0x07;
    uint8_t lfeon         = (data[1] >> 2) & 0x01;
    uint8_t bit_rate_code = ((data[1] & 0x03) << 3) | ((data[2] >> 5) & 0x07);
    if( fscod != 0x03 )
        lsmash_ifprintf( fp, indent, "fscod = %"PRIu8" (%"PRIu32" Hz)\n", fscod, ac3_sample_rate_table[fscod] );
    else
        lsmash_ifprintf( fp, indent, "fscod = 0x03 (reserved)\n" );
    lsmash_ifprintf( fp, indent, "bsid = %"PRIu8"\n", bsid );
    lsmash_ifprintf( fp, indent, "bsmod = %"PRIu8" (%s)\n", bsmod, bit_stream_mode[bsmod + (acmod == 0x01 ? 1 : acmod > 0x01 ? 2 : 0)] );
    lsmash_ifprintf( fp, indent, "acmod = %"PRIu8" (%s)\n", acmod, audio_coding_mode[acmod + (bsmod == 0x07 ? 8 : 0)] );
    lsmash_ifprintf( fp, indent, "lfeon = %s\n", lfeon ? "1 (LFE)" : "0" );
    static const uint32_t bit_rate[] =
        {
            32,   40,  48,  56,  64,  80,  96, 112, 128,
            160, 192, 224, 256, 320, 384, 448, 512, 576, 640,
            0   /* undefined */
        };
    lsmash_ifprintf( fp, indent, "bit_rate_code = 0x%02"PRIx8" (%"PRIu32" kbit/s)\n", bit_rate_code, bit_rate[bit_rate_code] );
    lsmash_ifprintf( fp, indent, "reserved = 0x%02"PRIx8"\n", data[2] & 0x1F );
    return 0;
}

#undef AC3_SPECIFIC_BOX_LENGTH

/***************************************************************************
    Enhanced AC-3 tools
    ETSI TS 102 366 V1.2.1 (2008-08)
***************************************************************************/

uint8_t *lsmash_create_eac3_specific_info( lsmash_eac3_specific_parameters_t *param, uint32_t *data_length )
{
#define EAC3_SPECIFIC_BOX_MAX_LENGTH 42
    if( param->num_ind_sub > 7 )
        return NULL;
    lsmash_bits_t bits = { 0 };
    lsmash_bs_t   bs   = { 0 };
    lsmash_bits_init( &bits, &bs );
    uint8_t buffer[EAC3_SPECIFIC_BOX_MAX_LENGTH] = { 0 };
    bs.buffer.data  = buffer;
    bs.buffer.alloc = EAC3_SPECIFIC_BOX_MAX_LENGTH;
    lsmash_bits_put( &bits, 32, 0 );                            /* box size */
    lsmash_bits_put( &bits, 32, ISOM_BOX_TYPE_DEC3.fourcc );    /* box type: 'dec3' */
    lsmash_bits_put( &bits, 13, param->data_rate );             /* data_rate; setup by isom_update_bitrate_description */
    lsmash_bits_put( &bits, 3, param->num_ind_sub );
    /* Apparently, the condition of this loop defined in ETSI TS 102 366 V1.2.1 (2008-08) is wrong. */
    for( int i = 0; i <= param->num_ind_sub; i++ )
    {
        lsmash_eac3_substream_info_t *independent_info = &param->independent_info[i];
        lsmash_bits_put( &bits, 2, independent_info->fscod );
        lsmash_bits_put( &bits, 5, independent_info->bsid );
        lsmash_bits_put( &bits, 5, independent_info->bsmod );
        lsmash_bits_put( &bits, 3, independent_info->acmod );
        lsmash_bits_put( &bits, 1, independent_info->lfeon );
        lsmash_bits_put( &bits, 3, 0 );          /* reserved */
        lsmash_bits_put( &bits, 4, independent_info->num_dep_sub );
        if( independent_info->num_dep_sub > 0 )
            lsmash_bits_put( &bits, 9, independent_info->chan_loc );
        else
            lsmash_bits_put( &bits, 1, 0 );      /* reserved */
    }
    uint8_t *data = lsmash_bits_export_data( &bits, data_length );
    lsmash_bits_empty( &bits );
    /* Update box size. */
    LSMASH_SET_BE32( data, *data_length );
    return data;
#undef EAC3_SPECIFIC_BOX_MAX_LENGTH
}

/* Return -1 if incomplete Enhanced AC-3 sample is given. */
int lsmash_setup_eac3_specific_parameters_from_frame( lsmash_eac3_specific_parameters_t *param, uint8_t *data, uint32_t data_length )
{
    if( !data || data_length < 5 )
        return -1;
    lsmash_bits_t bits = { 0 };
    lsmash_bs_t   bs   = { 0 };
    uint8_t buffer[EAC3_MAX_SYNCFRAME_LENGTH] = { 0 };
    bs.buffer.data  = buffer;
    bs.buffer.alloc = EAC3_MAX_SYNCFRAME_LENGTH;
    eac3_info_t  handler = { { 0 } };
    eac3_info_t *info    = &handler;
    uint32_t overall_wasted_data_length = 0;
    info->buffer_pos = info->buffer;
    info->buffer_end = info->buffer;
    info->bits = &bits;
    lsmash_bits_init( &bits, &bs );
    while( 1 )
    {
        /* Check the remainder length of the input data.
         * If there is enough length, then parse the syncframe in it.
         * The length 5 is the required byte length to get frame size. */
        uint32_t remainder_length = info->buffer_end - info->buffer_pos;
        if( !info->no_more_read && remainder_length < EAC3_MAX_SYNCFRAME_LENGTH )
        {
            if( remainder_length )
                memmove( info->buffer, info->buffer_pos, remainder_length );
            uint32_t wasted_data_length = LSMASH_MIN( data_length, EAC3_MAX_SYNCFRAME_LENGTH );
            data_length -= wasted_data_length;
            memcpy( info->buffer + remainder_length, data + overall_wasted_data_length, wasted_data_length );
            overall_wasted_data_length += wasted_data_length;
            remainder_length           += wasted_data_length;
            info->buffer_pos = info->buffer;
            info->buffer_end = info->buffer + remainder_length;
            info->no_more_read = (data_length < 5);
        }
        if( remainder_length < 5 && info->no_more_read )
            goto setup_param;   /* No more valid data. */
        /* Parse syncframe. */
        IF_A52_SYNCWORD( info->buffer_pos )
            goto setup_param;
        info->frame_size = 0;
        if( eac3_parse_syncframe( info, info->buffer_pos, LSMASH_MIN( remainder_length, EAC3_MAX_SYNCFRAME_LENGTH ) ) )
            goto setup_param;
        if( remainder_length < info->frame_size )
            goto setup_param;
        int independent = info->strmtyp != 0x1;
        if( independent && info->substreamid == 0x0 )
        {
            if( info->number_of_audio_blocks == 6 )
            {
                /* Encountered the first syncframe of the next access unit. */
                info->number_of_audio_blocks = 0;
                goto setup_param;
            }
            else if( info->number_of_audio_blocks > 6 )
                goto setup_param;
            info->number_of_audio_blocks += eac3_audio_block_table[ info->numblkscod ];
            info->number_of_independent_substreams = 0;
        }
        else if( info->syncframe_count == 0 )
            /* The first syncframe in an AU must be independent and assigned substream ID 0. */
            return -2;
        if( independent )
            info->independent_info[info->number_of_independent_substreams ++].num_dep_sub = 0;
        else
            ++ info->independent_info[info->number_of_independent_substreams - 1].num_dep_sub;
        info->buffer_pos += info->frame_size;
        ++ info->syncframe_count;
    }
setup_param:
    if( info->number_of_independent_substreams == 0 || info->number_of_independent_substreams > 8 )
        return -1;
    if( !info->dec3_param_initialized )
        eac3_update_specific_param( info );
    *param = info->dec3_param;
    return info->number_of_audio_blocks == 6 ? 0 : -1;
}

uint16_t lsmash_eac3_get_chan_loc_from_chanmap( uint16_t chanmap )
{
    return ((chanmap & 0x7f8) >> 2) | ((chanmap & 0x2) >> 1);
}

static int eac3_check_syncframe_header( eac3_info_t *info )
{
    if( info->strmtyp == 0x3 )
        return -1;      /* unknown Stream type */
    lsmash_eac3_substream_info_t *substream_info;
    if( info->strmtyp != 0x1 )
        substream_info = &info->independent_info[ info->current_independent_substream_id ];
    else
        substream_info = &info->dependent_info;
    if( substream_info->fscod == 0x3 && substream_info->fscod2 == 0x3 )
        return -1;      /* unknown Sample Rate Code */
    if( substream_info->bsid < 10 || substream_info->bsid > 16 )
        return -1;      /* not EAC-3 */
    return 0;
}

int eac3_parse_syncframe( eac3_info_t *info, uint8_t *data, uint32_t data_length )
{
    lsmash_bits_t *bits = info->bits;
    if( lsmash_bits_import_data( bits, data, data_length ) )
        return -1;
    lsmash_bits_get( bits, 16 );                                                    /* syncword           (16) */
    info->strmtyp     = lsmash_bits_get( bits, 2 );                                 /* strmtyp            (2) */
    info->substreamid = lsmash_bits_get( bits, 3 );                                 /* substreamid        (3) */
    lsmash_eac3_substream_info_t *substream_info;
    if( info->strmtyp != 0x1 )
    {
        if( info->substreamid == 0x0 && info->number_of_independent_substreams )
            eac3_update_specific_param( info );
        info->current_independent_substream_id = info->substreamid;
        substream_info = &info->independent_info[ info->current_independent_substream_id ];
        substream_info->chan_loc = 0;
    }
    else
        substream_info = &info->dependent_info;
    info->frame_size = 2 * (lsmash_bits_get( bits, 11 ) + 1);                       /* frmsiz             (11) */
    substream_info->fscod = lsmash_bits_get( bits, 2 );                             /* fscod              (2) */
    if( substream_info->fscod == 0x3 )
    {
        substream_info->fscod2 = lsmash_bits_get( bits, 2 );                        /* fscod2             (2) */
        info->numblkscod = 0x3;
    }
    else
        info->numblkscod = lsmash_bits_get( bits, 2 );                              /* numblkscod         (2) */
    substream_info->acmod = lsmash_bits_get( bits, 3 );                             /* acmod              (3) */
    substream_info->lfeon = lsmash_bits_get( bits, 1 );                             /* lfeon              (1) */
    substream_info->bsid = lsmash_bits_get( bits, 5 );                              /* bsid               (5) */
    lsmash_bits_get( bits, 5 );                                                     /* dialnorm           (5) */
    if( lsmash_bits_get( bits, 1 ) )                                                /* compre             (1) */
        lsmash_bits_get( bits, 8 );                                                 /* compr              (8) */
    if( substream_info->acmod == 0x0 )
    {
        lsmash_bits_get( bits, 5 );                                                 /* dialnorm2          (5) */
        if( lsmash_bits_get( bits, 1 ) )                                            /* compre2            (1) */
            lsmash_bits_get( bits, 8 );                                             /* compr2             (8) */
    }
    if( info->strmtyp == 0x1 && lsmash_bits_get( bits, 1 ) )                        /* chanmape           (1) */
    {
        uint16_t chanmap = lsmash_bits_get( bits, 16 );                             /* chanmap            (16) */
        info->independent_info[ info->current_independent_substream_id ].chan_loc |= lsmash_eac3_get_chan_loc_from_chanmap( chanmap );
    }
    if( lsmash_bits_get( bits, 1 ) )                                                /* mixmdate           (1) */
    {
        if( substream_info->acmod > 0x2 )
            lsmash_bits_get( bits, 2 );                                             /* dmixmod            (2) */
        if( ((substream_info->acmod & 0x1) && (substream_info->acmod > 0x2)) || (substream_info->acmod & 0x4) )
            lsmash_bits_get( bits, 6 );                                             /* ltrt[c/sur]mixlev  (3)
                                                                                     * loro[c/sur]mixlev  (3) */
        if( substream_info->lfeon && lsmash_bits_get( bits, 1 ) )                   /* lfemixlevcode      (1) */
            lsmash_bits_get( bits, 5 );                                             /* lfemixlevcod       (5) */
        if( info->strmtyp == 0x0 )
        {
            if( lsmash_bits_get( bits, 1 ) )                                        /* pgmscle            (1) */
                lsmash_bits_get( bits, 6 );                                         /* pgmscl             (6) */
            if( substream_info->acmod == 0x0 && lsmash_bits_get( bits, 1 ) )        /* pgmscle2           (1) */
                lsmash_bits_get( bits, 6 );                                         /* pgmscl2            (6) */
            if( lsmash_bits_get( bits, 1 ) )                                        /* extpgmscle         (1) */
                lsmash_bits_get( bits, 6 );                                         /* extpgmscl          (6) */
            uint8_t mixdef = lsmash_bits_get( bits, 2 );                            /* mixdef             (2) */
            if( mixdef == 0x1 )
                lsmash_bits_get( bits, 5 );                                         /* premixcmpsel       (1)
                                                                                     * drcsrc             (1)
                                                                                     * premixcmpscl       (3) */
            else if( mixdef == 0x2 )
                lsmash_bits_get( bits, 12 );                                        /* mixdata            (12) */
            else if( mixdef == 0x3 )
            {
                uint8_t mixdeflen = lsmash_bits_get( bits, 5 );                     /* mixdeflen          (5) */
                lsmash_bits_get( bits, 8 * (mixdeflen + 2) );                       /* mixdata            (8 * (mixdeflen + 2))
                                                                                     * mixdatafill        (0-7) */
            }
            if( substream_info->acmod < 0x2 )
            {
                if( lsmash_bits_get( bits, 1 ) )                                    /* paninfoe           (1) */
                    lsmash_bits_get( bits, 14 );                                    /* panmean            (8)
                                                                                     * paninfo            (6) */
                if( substream_info->acmod == 0x0 && lsmash_bits_get( bits, 1 ) )    /* paninfo2e          (1) */
                    lsmash_bits_get( bits, 14 );                                    /* panmean2           (8)
                                                                                     * paninfo2           (6) */
            }
            if( lsmash_bits_get( bits, 1 ) )                                        /* frmmixcfginfoe     (1) */
            {
                if( info->numblkscod == 0x0 )
                    lsmash_bits_get( bits, 5 );                                     /* blkmixcfginfo[0]   (5) */
                else
                {
                    int number_of_blocks_per_syncframe = ((int []){ 1, 2, 3, 6 })[ info->numblkscod ];
                    for( int blk = 0; blk < number_of_blocks_per_syncframe; blk++ )
                        if( lsmash_bits_get( bits, 1 ) )                            /* blkmixcfginfoe     (1)*/
                            lsmash_bits_get( bits, 5 );                             /* blkmixcfginfo[blk] (5) */
                }
            }
        }
    }
    if( lsmash_bits_get( bits, 1 ) )                                                /* infomdate          (1) */
    {
        substream_info->bsmod = lsmash_bits_get( bits, 3 );                         /* bsmod              (3) */
        lsmash_bits_get( bits, 1 );                                                 /* copyrightb         (1) */
        lsmash_bits_get( bits, 1 );                                                 /* origbs             (1) */
        if( substream_info->acmod == 0x2 )
            lsmash_bits_get( bits, 4 );                                             /* dsurmod            (2)
                                                                                     * dheadphonmod       (2) */
        else if( substream_info->acmod >= 0x6 )
            lsmash_bits_get( bits, 2 );                                             /* dsurexmod          (2) */
        if( lsmash_bits_get( bits, 1 ) )                                            /* audprodie          (1) */
            lsmash_bits_get( bits, 8 );                                             /* mixlevel           (5)
                                                                                     * roomtyp            (2)
                                                                                     * adconvtyp          (1) */
        if( substream_info->acmod == 0x0 && lsmash_bits_get( bits, 1 ) )            /* audprodie2         (1) */
            lsmash_bits_get( bits, 8 );                                             /* mixlevel2          (5)
                                                                                     * roomtyp2           (2)
                                                                                     * adconvtyp2         (1) */
        if( substream_info->fscod < 0x3 )
            lsmash_bits_get( bits, 1 );                                             /* sourcefscod        (1) */
    }
    else
        substream_info->bsmod = 0;
    if( info->strmtyp == 0x0 && info->numblkscod != 0x3 )
        lsmash_bits_get( bits, 1 );                                                 /* convsync           (1) */
    if( info->strmtyp == 0x2 )
    {
        int blkid;
        if( info->numblkscod == 0x3 )
            blkid = 1;
        else
            blkid = lsmash_bits_get( bits, 1 );                                     /* blkid              (1) */
        if( blkid )
            lsmash_bits_get( bits, 6 );                                             /* frmsizecod         (6) */
    }
    if( lsmash_bits_get( bits, 1 ) )                                                /* addbsie            (1) */
    {
        uint8_t addbsil = lsmash_bits_get( bits, 6 );                               /* addbsil            (6) */
        lsmash_bits_get( bits, (addbsil + 1) * 8 );                                 /* addbsi             ((addbsil + 1) * 8) */
    }
    lsmash_bits_empty( bits );
    return eac3_check_syncframe_header( info );
}

void eac3_update_specific_param( eac3_info_t *info )
{
    lsmash_eac3_specific_parameters_t *param = &info->dec3_param;
    param->data_rate = 0;
    param->num_ind_sub = info->number_of_independent_substreams - 1;
    for( uint8_t i = 0; i <= param->num_ind_sub; i++ )
        param->independent_info[i] = info->independent_info[i];
    info->dec3_param_initialized = 1;
}

#define EAC3_SPECIFIC_BOX_MIN_LENGTH 13

int eac3_construct_specific_parameters( lsmash_codec_specific_t *dst, lsmash_codec_specific_t *src )
{
    assert( dst && dst->data.structured && src && src->data.unstructured );
    if( src->size < EAC3_SPECIFIC_BOX_MIN_LENGTH )
        return -1;
    lsmash_eac3_specific_parameters_t *param = (lsmash_eac3_specific_parameters_t *)dst->data.structured;
    uint8_t *data = src->data.unstructured;
    uint64_t size = LSMASH_GET_BE32( data );
    data += ISOM_BASEBOX_COMMON_SIZE;
    if( size == 1 )
    {
        size = LSMASH_GET_BE64( data );
        data += 8;
    }
    if( size != src->size )
        return -1;
    param->data_rate   = (data[0] << 5) | ((data[1] >> 3) & 0x1F);  /* XXXX XXXX XXXX Xxxx */
    param->num_ind_sub = data[1] & 0x07;                            /* xxxx xxxx xxxx xXXX */
    data += 2;
    size -= 2;
    for( int i = 0; i <= param->num_ind_sub; i++ )
    {
        if( size < 3 )
            return -1;
        lsmash_eac3_substream_info_t *independent_info = &param->independent_info[i];
        independent_info->fscod       = (data[0] >> 6) & 0x03;                              /* XXxx xxxx xxxx xxxx xxxx xxxx */
        independent_info->bsid        = (data[0] >> 1) & 0x1F;                              /* xxXX XXXx xxxx xxxx xxxx xxxx */
        independent_info->bsmod       = ((data[0] & 0x01) << 4) | ((data[1] >> 4) & 0x0F);  /* xxxx xxxX XXXX xxxx xxxx xxxx */
        independent_info->acmod       = (data[1] >> 1) & 0x07;                              /* xxxx xxxx xxxx XXXx xxxx xxxx */
        independent_info->lfeon       = data[1] & 0x01;                                     /* xxxx xxxx xxxx xxxX xxxx xxxx */
        independent_info->num_dep_sub = (data[2] >> 1) & 0x0F;                              /* xxxx xxxx xxxx xxxx xxxX XXXx */
        data += 3;
        size -= 3;
        if( independent_info->num_dep_sub > 0 )
        {
            if( size < 1 )
                return -1;
            independent_info->chan_loc = ((data[-1] & 0x01) << 8) | data[0];    /* xxxx xxxX XXXX XXXX */
            data += 1;
            size -= 1;
        }
    }
    return 0;
}

int eac3_print_codec_specific( FILE *fp, lsmash_file_t *file, isom_box_t *box, int level )
{
    assert( fp && file && box && (box->manager & LSMASH_BINARY_CODED_BOX) );
    int indent = level;
    lsmash_ifprintf( fp, indent++, "[%s: EC3 Specific Box]\n", isom_4cc2str( box->type.fourcc ) );
    lsmash_ifprintf( fp, indent, "position = %"PRIu64"\n", box->pos );
    lsmash_ifprintf( fp, indent, "size = %"PRIu64"\n", box->size );
    if( box->size < EAC3_SPECIFIC_BOX_MIN_LENGTH )
        return -1;
    uint8_t *data = box->binary;
    isom_skip_box_common( &data );
    lsmash_ifprintf( fp, indent, "data_rate = %"PRIu16" kbit/s\n", (data[0] << 5) | ((data[1] >> 3) & 0x1F) );
    uint8_t num_ind_sub = data[1] & 0x07;
    lsmash_ifprintf( fp, indent, "num_ind_sub = %"PRIu8"\n", num_ind_sub );
    data += 2;
    for( int i = 0; i <= num_ind_sub; i++ )
    {
        lsmash_ifprintf( fp, indent, "independent_substream[%d]\n", i );
        int sub_indent = indent + 1;
        uint8_t fscod       = (data[0] >> 6) & 0x03;
        uint8_t bsid        = (data[0] >> 1) & 0x1F;
        uint8_t bsmod       = ((data[0] & 0x01) << 4) | ((data[1] >> 4) & 0x0F);
        uint8_t acmod       = (data[1] >> 1) & 0x07;
        uint8_t lfeon       = data[1] & 0x01;
        uint8_t num_dep_sub = (data[2] >> 1) & 0x0F;
        if( fscod != 0x03 )
            lsmash_ifprintf( fp, sub_indent, "fscod = %"PRIu8" (%"PRIu32" Hz)\n", fscod, ac3_sample_rate_table[fscod] );
        else
            lsmash_ifprintf( fp, sub_indent, "fscod = 0x03 (reduced sample rate)\n" );
        lsmash_ifprintf( fp, sub_indent, "bsid = %"PRIu8"\n", bsid );
        if( bsmod < 0x08 )
            lsmash_ifprintf( fp, sub_indent, "bsmod = %"PRIu8" (%s)\n", bsmod, bit_stream_mode[bsmod + (acmod == 0x01 ? 1 : acmod > 0x01 ? 2 : 0)] );
        else
            lsmash_ifprintf( fp, sub_indent, "bsmod = %"PRIu8" (Undefined service)\n" );
        lsmash_ifprintf( fp, sub_indent, "acmod = %"PRIu8" (%s)\n", acmod, audio_coding_mode[acmod + (bsmod == 0x07 ? 8 : 0)] );
        lsmash_ifprintf( fp, sub_indent, "lfeon = %s\n", lfeon ? "1 (LFE)" : "0" );
        lsmash_ifprintf( fp, sub_indent, "num_dep_sub = %"PRIu8"\n", num_dep_sub );
        data += 3;
        if( num_dep_sub > 0 )
        {
            static const char *channel_location[] =
                {
                    "LFE2",
                    "Cvh",
                    "Lvh/Rvh pair",
                    "Lw/Rw pair",
                    "Lsd/Rsd pair",
                    "Ts",
                    "Cs",
                    "Lrs/Rrs pair",
                    "Lc/Rc pair"
                };
            uint16_t chan_loc = ((data[-1] & 0x01) << 8) | data[0];
            lsmash_ifprintf( fp, sub_indent, "chan_loc = 0x%04"PRIu16"\n", chan_loc );
            for( int j = 0; j < 9; j++ )
                if( (chan_loc >> j & 0x01) )
                    lsmash_ifprintf( fp, sub_indent + 1, "%s\n", channel_location[j] );
            data += 1;
        }
        else
            lsmash_ifprintf( fp, sub_indent, "reserved = %"PRIu8"\n", data[2] & 0x01 );
    }
    return 0;
}

#undef EAC3_SPECIFIC_BOX_MIN_LENGTH
