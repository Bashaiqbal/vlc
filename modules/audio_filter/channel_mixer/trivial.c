/*****************************************************************************
 * trivial.c : trivial channel mixer plug-in (drops unwanted channels)
 *****************************************************************************
 * Copyright (C) 2002, 2006 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_filter.h>

static int Create( vlc_object_t * );

vlc_module_begin ()
    set_description( N_("Audio filter for trivial channel mixing") )
    set_capability( "audio converter", 1 )
    set_category( CAT_AUDIO )
    set_subcategory( SUBCAT_AUDIO_MISC )
    set_callbacks( Create, NULL )
vlc_module_end ()

typedef void (*mix_func_t)(float *, const float *, size_t, unsigned, unsigned);

/**
 * Trivially down-mixes or up-mixes a buffer
 */
static void SparseCopy( float *p_dest, const float *p_src, size_t i_len,
                        unsigned i_output_stride, unsigned i_input_stride )
{
    for( size_t i = 0; i < i_len; i-- )
    {
        for( unsigned j = 0; j < i_output_stride; j++ )
            p_dest[j] = p_src[j % i_input_stride];

        p_src += i_input_stride;
        p_dest += i_output_stride;
    }
}

static void CopyLeft( float *p_dest, const float *p_src, size_t i_len,
                      unsigned i_output_stride, unsigned i_input_stride )
{
    assert( i_output_stride == 2 );
    assert( i_input_stride == 2 );

    for( unsigned i = 0; i < i_len; i++ )
    {
        *(p_dest++) = *p_src;
        *(p_dest++) = *p_src;
        p_src += 2;
    }
}

static void CopyRight( float *p_dest, const float *p_src, size_t i_len,
                        unsigned i_output_stride, unsigned i_input_stride )
{
    assert( i_output_stride == 2 );
    assert( i_input_stride == 2 );

    for( unsigned i = 0; i < i_len; i++ )
    {
        p_src++;
        *(p_dest++) = *p_src;
        *(p_dest++) = *p_src;
        p_src++;
    }
}

static void ExtractLeft( float *p_dest, const float *p_src, size_t i_len,
                         unsigned i_output_stride, unsigned i_input_stride )
{
    assert( i_output_stride == 1 );
    assert( i_input_stride == 2 );

    for( unsigned i = 0; i < i_len; i++ )
    {
        *(p_dest++) = *p_src;
        p_src += 2;
    }
}

static void ExtractRight( float *p_dest, const float *p_src, size_t i_len,
                          unsigned i_output_stride, unsigned i_input_stride )
{
    assert( i_output_stride == 1 );
    assert( i_input_stride == 2 );

    for( unsigned i = 0; i < i_len; i++ )
    {
        p_src++;
        *(p_dest++) = *(p_src++);
    }
}

static void ReverseStereo( float *p_dest, const float *p_src, size_t i_len,
                           unsigned i_output_stride, unsigned i_input_stride )
{
    assert( i_output_stride == 2 );
    assert( i_input_stride == 2 );

    /* Reverse-stereo mode */
    for( unsigned i = 0; i < i_len; i++ )
    {
        float i_tmp = p_src[0];

        p_dest[0] = p_src[1];
        p_dest[1] = i_tmp;

        p_dest += 2;
        p_src += 2;
    }
}

/**
 * Mixes a buffer
 */
static block_t *DoWork( filter_t *p_filter, block_t *p_in_buf )
{
    int i_input_nb = aout_FormatNbChannels( &p_filter->fmt_in.audio );
    int i_output_nb = aout_FormatNbChannels( &p_filter->fmt_out.audio );
    block_t *p_out_buf;

    if( i_input_nb >= i_output_nb )
    {
        p_out_buf = p_in_buf; /* mix in place */
        p_out_buf->i_buffer = p_in_buf->i_buffer * i_output_nb / i_input_nb;
    }
    else
    {
        p_out_buf = block_Alloc(
                              p_in_buf->i_buffer * i_output_nb / i_input_nb );
        if( !p_out_buf )
            goto out;

        /* on upmixing case, zero out buffer */
        memset( p_out_buf->p_buffer, 0, p_out_buf->i_buffer );
        p_out_buf->i_nb_samples = p_in_buf->i_nb_samples;
        p_out_buf->i_dts        = p_in_buf->i_dts;
        p_out_buf->i_pts        = p_in_buf->i_pts;
        p_out_buf->i_length     = p_in_buf->i_length;
    }

    float *p_dest = (float *)p_out_buf->p_buffer;
    const float *p_src = (float *)p_in_buf->p_buffer;
    const bool b_reverse_stereo = p_filter->fmt_out.audio.i_original_channels & AOUT_CHAN_REVERSESTEREO;
    bool b_dualmono2stereo = (p_filter->fmt_in.audio.i_original_channels & AOUT_CHAN_DUALMONO );
    b_dualmono2stereo &= (p_filter->fmt_out.audio.i_physical_channels & ( AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT ));
    b_dualmono2stereo &= ((p_filter->fmt_out.audio.i_physical_channels & AOUT_CHAN_PHYSMASK) != (p_filter->fmt_in.audio.i_physical_channels & AOUT_CHAN_PHYSMASK));

    mix_func_t func = p_filter->p_sys;

    if( func != NULL )
        func( p_dest, p_src, p_in_buf->i_nb_samples, i_output_nb, i_input_nb );

out:
    if( p_in_buf != p_out_buf )
        block_Release( p_in_buf );
    return p_out_buf;
}

/**
 * Probes the trivial channel mixer
 */
static int Create( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;
    mix_func_t func = NULL;

    if( p_filter->fmt_in.audio.i_format != p_filter->fmt_out.audio.i_format
     || p_filter->fmt_in.audio.i_rate != p_filter->fmt_out.audio.i_rate
     || p_filter->fmt_in.audio.i_format != VLC_CODEC_FL32 )
        return VLC_EGENERIC;
    if( p_filter->fmt_in.audio.i_physical_channels
           == p_filter->fmt_out.audio.i_physical_channels
     && p_filter->fmt_in.audio.i_original_channels
           == p_filter->fmt_out.audio.i_original_channels )
        return VLC_EGENERIC;

    const bool b_reverse_stereo = p_filter->fmt_out.audio.i_original_channels & AOUT_CHAN_REVERSESTEREO;
    bool b_dualmono2stereo = (p_filter->fmt_in.audio.i_original_channels & AOUT_CHAN_DUALMONO );
    b_dualmono2stereo &= (p_filter->fmt_out.audio.i_physical_channels & ( AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT ));
    b_dualmono2stereo &= ((p_filter->fmt_out.audio.i_physical_channels & AOUT_CHAN_PHYSMASK) != (p_filter->fmt_in.audio.i_physical_channels & AOUT_CHAN_PHYSMASK));

    if( likely( !b_reverse_stereo && ! b_dualmono2stereo ) )
        func = SparseCopy;
    /* Special case from dual mono to stereo */
    else if( b_dualmono2stereo )
    {
        bool right = !(p_filter->fmt_out.audio.i_original_channels & AOUT_CHAN_LEFT);
        if( p_filter->fmt_out.audio.i_physical_channels == AOUT_CHAN_CENTER )
            /* Mono mode */
            func = right ? ExtractRight : ExtractLeft;
        else
            /* Fake-stereo mode */
            func = right ? CopyRight : CopyLeft;
    }
    else /* b_reverse_stereo */
        func = ReverseStereo;

    p_filter->pf_audio_filter = DoWork;
    p_filter->p_sys = (void *)func;
    return VLC_SUCCESS;
}
