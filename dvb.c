/*****************************************************************************
 * dvb.c: linux-dvb input for DVBlast
 *****************************************************************************
 * Copyright (C) 2008-2010 VideoLAN
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>

/* DVB Card Drivers */
#include <linux/dvb/version.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/ca.h>

#include "dvblast.h"
#include "en50221.h"
#include "comm.h"

#include <bitstream/common.h>

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
#define DVR_READ_TIMEOUT 30000000 /* 30 s */
#define CA_POLL_PERIOD 100000 /* 100 ms */
#define MAX_READ_ONCE 50
#define DVR_BUFFER_SIZE 40*188*1024 /* bytes */

static int i_frontend, i_dvr;
static fe_status_t i_last_status;
static mtime_t i_frontend_timeout;
static mtime_t i_last_packet = 0;
static mtime_t i_ca_next_event = 0;
static block_t *p_freelist = NULL;

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static block_t *DVRRead( void );
static void FrontendPoll( void );
static void FrontendSet( bool b_reset );

/*****************************************************************************
 * dvb_Open
 *****************************************************************************/
void dvb_Open( void )
{
    char psz_tmp[128];

    msg_Dbg( NULL, "using linux-dvb API version %d", DVB_API_VERSION );

    i_wallclock = mdate();

    sprintf( psz_tmp, "/dev/dvb/adapter%d/frontend%d", i_adapter, i_fenum );
    if( (i_frontend = open(psz_tmp, O_RDWR | O_NONBLOCK)) < 0 )
    {
        msg_Err( NULL, "opening device %s failed (%s)", psz_tmp,
                 strerror(errno) );
        exit(1);
    }

    FrontendSet(true);

    sprintf( psz_tmp, "/dev/dvb/adapter%d/dvr%d", i_adapter, i_fenum );

    if( (i_dvr = open(psz_tmp, O_RDONLY | O_NONBLOCK)) < 0 )
    {
        msg_Err( NULL, "opening device %s failed (%s)", psz_tmp,
                 strerror(errno) );
        exit(1);
    }

    if ( ioctl( i_dvr, DMX_SET_BUFFER_SIZE, DVR_BUFFER_SIZE ) < 0 )
    {
        msg_Warn( NULL, "couldn't set %s buffer size (%s)", psz_tmp,
                 strerror(errno) );
    }

    en50221_Init();
    i_ca_next_event = mdate() + CA_POLL_PERIOD;
}

/*****************************************************************************
 * dvb_Reset
 *****************************************************************************/
void dvb_Reset( void )
{
    FrontendSet(true);
}

/*****************************************************************************
 * dvb_Read
 *****************************************************************************/
block_t *dvb_Read( mtime_t i_poll_timeout )
{
    struct pollfd ufds[4];
    int i_ret, i_nb_fd = 2;
    block_t *p_blocks = NULL;

    memset( ufds, 0, sizeof(ufds) );
    ufds[0].fd = i_dvr;
    ufds[0].events = POLLIN;
    ufds[1].fd = i_frontend;
    ufds[1].events = POLLERR | POLLPRI;
    if ( i_comm_fd != -1 )
    {
        ufds[i_nb_fd].fd = i_comm_fd;
        ufds[i_nb_fd].events = POLLIN;
        i_nb_fd++;
    }
    if ( i_ca_handle && i_ca_type == CA_CI_LINK )
    {
        ufds[i_nb_fd].fd = i_ca_handle;
        ufds[i_nb_fd].events = POLLIN;
        i_nb_fd++;
    }

    i_ret = poll( ufds, i_nb_fd, (i_poll_timeout + 999) / 1000 );

    i_wallclock = mdate();

    if ( i_ret < 0 )
    {
        if( errno != EINTR )
            msg_Err( NULL, "poll error: %s", strerror(errno) );
        return NULL;
    }

    if ( ufds[0].revents )
    {
        p_blocks = DVRRead();
        i_wallclock = mdate();
    }

    if ( p_blocks != NULL )
        i_last_packet = i_wallclock;
    else if ( !i_frontend_timeout
                && i_wallclock > i_last_packet + DVR_READ_TIMEOUT )
    {
        msg_Warn( NULL, "no DVR output, resetting" );
        FrontendSet(false);
        en50221_Reset();
    }

    if ( i_ca_handle && i_ca_type == CA_CI_LINK )
    {
        if ( ufds[i_nb_fd - 1].revents )
        {
            en50221_Read();
            i_ca_next_event = i_wallclock + CA_POLL_PERIOD;
        }
        else if ( i_wallclock > i_ca_next_event )
        {
            en50221_Poll();
            i_ca_next_event = i_wallclock + CA_POLL_PERIOD;
        }
    }

    if ( ufds[1].revents )
        FrontendPoll();

    if ( i_frontend_timeout && i_wallclock > i_frontend_timeout )
    {
        if ( i_quit_timeout_duration )
        {
            msg_Err( NULL, "no lock" );
            exit(EXIT_STATUS_FRONTEND_TIMEOUT);
        }
        msg_Warn( NULL, "no lock, tuning again" );
        FrontendSet(false);
    }

    if ( i_comm_fd != -1 && ufds[2].revents )
        comm_Read();

    return p_blocks;
}

/*****************************************************************************
 * DVRRead
 *****************************************************************************/
static block_t *DVRRead( void )
{
    int i, i_len;
    block_t *p_ts = p_freelist, **pp_current = &p_ts;
    struct iovec p_iov[MAX_READ_ONCE];

    for ( i = 0; i < MAX_READ_ONCE; i++ )
    {
        if ( (*pp_current) == NULL ) *pp_current = block_New();
        p_iov[i].iov_base = (*pp_current)->p_ts;
        p_iov[i].iov_len = TS_SIZE;
        pp_current = &(*pp_current)->p_next;
    }

    if ( (i_len = readv(i_dvr, p_iov, MAX_READ_ONCE)) < 0 )
    {
        msg_Err( NULL, "couldn't read from DVR device (%s)",
                 strerror(errno) );
        i_len = 0;
    }
    i_len /= TS_SIZE;

    pp_current = &p_ts;
    while ( i_len && *pp_current )
    {
        pp_current = &(*pp_current)->p_next;
        i_len--;
    }

    p_freelist = *pp_current;
    *pp_current = NULL;

    return p_ts;
}


/*
 * Demux
 */

/*****************************************************************************
 * dvb_SetFilter : controls the demux to add a filter
 *****************************************************************************/
int dvb_SetFilter( uint16_t i_pid )
{
    struct dmx_pes_filter_params s_filter_params;
    char psz_tmp[128];
    int i_fd;

    sprintf( psz_tmp, "/dev/dvb/adapter%d/demux%d", i_adapter, i_fenum );
    if( (i_fd = open(psz_tmp, O_RDWR)) < 0 )
    {
        msg_Err( NULL, "DMXSetFilter: opening device failed (%s)",
                 strerror(errno) );
        return -1;
    }

    s_filter_params.pid      = i_pid;
    s_filter_params.input    = DMX_IN_FRONTEND;
    s_filter_params.output   = DMX_OUT_TS_TAP;
    s_filter_params.flags    = DMX_IMMEDIATE_START;
    s_filter_params.pes_type = DMX_PES_OTHER;

    if ( ioctl( i_fd, DMX_SET_PES_FILTER, &s_filter_params ) < 0 )
    {
        msg_Err( NULL, "failed setting filter on %d (%s)", i_pid,
                 strerror(errno) );
        close( i_fd );
        return -1;
    }

    msg_Dbg( NULL, "setting filter on PID %d", i_pid );

    return i_fd;
}

/*****************************************************************************
 * dvb_UnsetFilter : removes a filter
 *****************************************************************************/
void dvb_UnsetFilter( int i_fd, uint16_t i_pid )
{
    if ( ioctl( i_fd, DMX_STOP ) < 0 )
        msg_Err( NULL, "DMX_STOP failed (%s)", strerror(errno) );
    else
        msg_Dbg( NULL, "unsetting filter on PID %d", i_pid );

    close( i_fd );
}


/*
 * Frontend
 */

/*****************************************************************************
 * FrontendPoll : Poll for frontend events
 *****************************************************************************/
static void FrontendPoll( void )
{
    struct dvb_frontend_event event;
    fe_status_t i_status, i_diff;

    for( ;; )
    {
        int i_ret = ioctl( i_frontend, FE_GET_EVENT, &event );

        if( i_ret < 0 )
        {
            if( errno == EWOULDBLOCK )
                return; /* no more events */

            msg_Err( NULL, "reading frontend event failed (%d) %s",
                     i_ret, strerror(errno) );
            return;
        }

        i_status = event.status;
        i_diff = i_status ^ i_last_status;
        i_last_status = i_status;

        {
#define IF_UP( x )                                                          \
        }                                                                   \
        if ( i_diff & (x) )                                                 \
        {                                                                   \
            if ( i_status & (x) )

            IF_UP( FE_HAS_SIGNAL )
                msg_Dbg( NULL, "frontend has acquired signal" );
            else
                msg_Dbg( NULL, "frontend has lost signal" );

            IF_UP( FE_HAS_CARRIER )
                msg_Dbg( NULL, "frontend has acquired carrier" );
            else
                msg_Dbg( NULL, "frontend has lost carrier" );

            IF_UP( FE_HAS_VITERBI )
                msg_Dbg( NULL, "frontend has acquired stable FEC" );
            else
                msg_Dbg( NULL, "frontend has lost FEC" );

            IF_UP( FE_HAS_SYNC )
                msg_Dbg( NULL, "frontend has acquired sync" );
            else
                msg_Dbg( NULL, "frontend has lost sync" );

            IF_UP( FE_HAS_LOCK )
            {
                int32_t i_value = 0;
                msg_Dbg( NULL, "frontend has acquired lock" );
                switch (i_print_type) {
                case PRINT_XML:
                    printf("<STATUS type=\"lock\" status=\"1\"/>\n");
                    break;
                default:
                    printf("frontend has acquired lock\n" );
                }
                i_frontend_timeout = 0;
                i_last_packet = i_wallclock;

                if ( i_quit_timeout_duration && !i_quit_timeout )
                    i_quit_timeout = i_wallclock + i_quit_timeout_duration;

                /* Read some statistics */
                if( ioctl( i_frontend, FE_READ_BER, &i_value ) >= 0 )
                    msg_Dbg( NULL, "- Bit error rate: %d", i_value );
                if( ioctl( i_frontend, FE_READ_SIGNAL_STRENGTH, &i_value ) >= 0 )
                    msg_Dbg( NULL, "- Signal strength: %d", i_value );
                if( ioctl( i_frontend, FE_READ_SNR, &i_value ) >= 0 )
                    msg_Dbg( NULL, "- SNR: %d", i_value );
            }
            else
            {
                msg_Dbg( NULL, "frontend has lost lock" );
                switch (i_print_type) {
                case PRINT_XML:
                    printf("<STATUS type=\"lock\" status=\"0\"/>\n");
                    break;
                default:
                    printf("frontend has lost lock\n" );
                }
                i_frontend_timeout = i_wallclock + i_frontend_timeout_duration;
            }

            IF_UP( FE_REINIT )
            {
                /* The frontend was reinited. */
                msg_Warn( NULL, "reiniting frontend");
                FrontendSet(true);
            }
        }
#undef IF_UP
    }
}

static int FrontendDoDiseqc(void)
{
    fe_sec_voltage_t fe_voltage;
    fe_sec_tone_mode_t fe_tone;
    int bis_frequency;

    switch ( i_voltage )
    {
        case 0: fe_voltage = SEC_VOLTAGE_OFF; break;
        default:
        case 13: fe_voltage = SEC_VOLTAGE_13; break;
        case 18: fe_voltage = SEC_VOLTAGE_18; break;
    }

    fe_tone = b_tone ? SEC_TONE_ON : SEC_TONE_OFF;

    /* Automatic mode. */
    if ( i_frequency >= 950000 && i_frequency <= 2150000 )
    {
        msg_Dbg( NULL, "frequency %d is in IF-band", i_frequency );
        bis_frequency = i_frequency;
    }
    else if ( i_frequency >= 2500000 && i_frequency <= 2700000 )
    {
        msg_Dbg( NULL, "frequency %d is in S-band", i_frequency );
        bis_frequency = 3650000 - i_frequency;
    }
    else if ( i_frequency >= 3400000 && i_frequency <= 4200000 )
    {
        msg_Dbg( NULL, "frequency %d is in C-band (lower)", i_frequency );
        bis_frequency = 5150000 - i_frequency;
    }
    else if ( i_frequency >= 4500000 && i_frequency <= 4800000 )
    {
        msg_Dbg( NULL, "frequency %d is in C-band (higher)", i_frequency );
        bis_frequency = 5950000 - i_frequency;
    }
    else if ( i_frequency >= 10700000 && i_frequency < 11700000 )
    {
        msg_Dbg( NULL, "frequency %d is in Ku-band (lower)",
                 i_frequency );
        bis_frequency = i_frequency - 9750000;
    }
    else if ( i_frequency >= 11700000 && i_frequency <= 13250000 )
    {
        msg_Dbg( NULL, "frequency %d is in Ku-band (higher)",
                 i_frequency );
        bis_frequency = i_frequency - 10600000;
        fe_tone = SEC_TONE_ON;
    }
    else
    {
        msg_Err( NULL, "frequency %d is out of any known band",
                 i_frequency );
        exit(1);
    }

    /* Switch off continuous tone. */
    if ( ioctl( i_frontend, FE_SET_TONE, SEC_TONE_OFF ) < 0 )
    {
        msg_Err( NULL, "FE_SET_TONE failed (%s)", strerror(errno) );
        exit(1);
    }

    /* Configure LNB voltage. */
    if ( ioctl( i_frontend, FE_SET_VOLTAGE, fe_voltage ) < 0 )
    {
        msg_Err( NULL, "FE_SET_VOLTAGE failed (%s)", strerror(errno) );
        exit(1);
    }

    /* Wait for at least 15 ms. Currently 100 ms because of broken drivers. */
    msleep(100000);

    /* Diseqc */
    if ( i_satnum > 0 && i_satnum < 5 )
    {
        /* digital satellite equipment control,
         * specification is available from http://www.eutelsat.com/
         */

        struct dvb_diseqc_master_cmd cmd =
            { {0xe0, 0x10, 0x38, 0xf0, 0x00, 0x00}, 4};
        cmd.msg[3] = 0xf0 /* reset bits */
                          | ((i_satnum - 1) << 2)
                          | (fe_voltage == SEC_VOLTAGE_13 ? 0 : 2)
                          | (fe_tone == SEC_TONE_ON ? 1 : 0);

        if( ioctl( i_frontend, FE_DISEQC_SEND_MASTER_CMD, &cmd ) < 0 )
        {
            msg_Err( NULL, "ioctl FE_SEND_MASTER_CMD failed (%s)",
                     strerror(errno) );
            exit(1);
        }
        msleep(100000); /* Should be 15 ms. */

        /* Do it again just to be sure. */
        if( ioctl( i_frontend, FE_DISEQC_SEND_MASTER_CMD, &cmd ) < 0 )
        {
            msg_Err( NULL, "ioctl FE_SEND_MASTER_CMD failed (%s)",
                     strerror(errno) );
            exit(1);
        }
        msleep(100000); /* Again, should be 15 ms */
    }
    else if ( i_satnum == 0xA || i_satnum == 0xB )
    {
        /* A or B simple diseqc ("diseqc-compatible") */
        if( ioctl( i_frontend, FE_DISEQC_SEND_BURST,
                   i_satnum == 0xB ? SEC_MINI_B : SEC_MINI_A ) < 0 )
        {
            msg_Err( NULL, "ioctl FE_SEND_BURST failed (%m)", strerror(errno) );
            exit(1);
        }
        msleep(100000); /* ... */
    }

    else if ( ioctl( i_frontend, FE_SET_TONE, fe_tone ) < 0 )
    {
        msg_Err( NULL, "FE_SET_TONE failed (%s)", strerror(errno) );
        exit(1);
    }

    msleep(100000); /* ... */

    msg_Dbg( NULL, "configuring LNB to v=%d p=%d satnum=%x",
             i_voltage, b_tone, i_satnum );
    return bis_frequency;
}

#define DVBAPI_VERSION ((DVB_API_VERSION)*100+(DVB_API_VERSION_MINOR))

#if DVB_API_VERSION >= 5

/*****************************************************************************
 * Helper functions for S2API
 *****************************************************************************/
static fe_spectral_inversion_t GetInversion(void)
{
    switch ( i_inversion )
    {
        case 0:  return INVERSION_OFF;
        case 1:  return INVERSION_ON;
        default:
            msg_Warn( NULL, "invalid inversion %d", i_inversion );
        case -1: return INVERSION_AUTO;
    }
}

static fe_code_rate_t GetFEC(fe_caps_t fe_caps, int i_fec_value)
{
#define GET_FEC_INNER(fec, val)                                             \
    if ( (fe_caps & FE_CAN_##fec) && (i_fec_value == val) )                 \
       return fec;

    GET_FEC_INNER(FEC_AUTO, 999);
    GET_FEC_INNER(FEC_AUTO, -1);
    if (i_fec_value == 0)
        return FEC_NONE;
    GET_FEC_INNER(FEC_1_2, 12);
    GET_FEC_INNER(FEC_2_3, 23);
    GET_FEC_INNER(FEC_3_4, 34);
    if (i_fec_value == 35)
        return FEC_3_5;
    GET_FEC_INNER(FEC_4_5, 45);
    GET_FEC_INNER(FEC_5_6, 56);
    GET_FEC_INNER(FEC_6_7, 67);
    GET_FEC_INNER(FEC_7_8, 78);
    GET_FEC_INNER(FEC_8_9, 89);
    if (i_fec_value == 910)
        return FEC_9_10;

#undef GET_FEC_INNER
    msg_Warn(NULL, "invalid FEC %d", i_fec_value );
    return FEC_AUTO;
}

#define GetFECInner(caps) GetFEC(caps, i_fec)
#define GetFECLP(caps) GetFEC(caps, i_fec_lp)

static fe_modulation_t GetModulation(void)
{
#define GET_MODULATION( mod )                                               \
    if ( !strcasecmp( psz_modulation, #mod ) )                              \
        return mod;

    GET_MODULATION(QPSK);
    GET_MODULATION(QAM_16);
    GET_MODULATION(QAM_32);
    GET_MODULATION(QAM_64);
    GET_MODULATION(QAM_128);
    GET_MODULATION(QAM_256);
    GET_MODULATION(QAM_AUTO);
    GET_MODULATION(VSB_8);
    GET_MODULATION(VSB_16);
    GET_MODULATION(PSK_8);
    GET_MODULATION(APSK_16);
    GET_MODULATION(APSK_32);
    GET_MODULATION(DQPSK);

#undef GET_MODULATION
    msg_Err( NULL, "invalid modulation %s", psz_modulation );
    exit(1);
}

static fe_pilot_t GetPilot(void)
{
    switch ( i_pilot )
    {
        case 0:  return PILOT_OFF;
        case 1:  return PILOT_ON;
        default:
            msg_Warn( NULL, "invalid pilot %d", i_pilot );
        case -1: return PILOT_AUTO;
    }
}

static fe_rolloff_t GetRollOff(void)
{
    switch ( i_rolloff )
    {
        case -1:
        case  0: return ROLLOFF_AUTO;
        case 20: return ROLLOFF_20;
        case 25: return ROLLOFF_25;
        default:
            msg_Warn( NULL, "invalid rolloff %d", i_rolloff );
        case 35: return ROLLOFF_35;
    }
}

static fe_guard_interval_t GetGuard(void)
{
    switch ( i_guard )
    {
        case 32: return GUARD_INTERVAL_1_32;
        case 16: return GUARD_INTERVAL_1_16;
        case  8: return GUARD_INTERVAL_1_8;
        case  4: return GUARD_INTERVAL_1_4;
        default:
            msg_Warn( NULL, "invalid guard interval %d", i_guard );
        case -1:
        case  0: return GUARD_INTERVAL_AUTO;
    }
}

static fe_transmit_mode_t GetTransmission(void)
{
    switch ( i_transmission )
    {
        case 2: return TRANSMISSION_MODE_2K;
        case 8: return TRANSMISSION_MODE_8K;
#ifdef TRANSMISSION_MODE_4K
        case 4: return TRANSMISSION_MODE_4K;
#endif
        default:
            msg_Warn( NULL, "invalid tranmission mode %d", i_transmission );
        case -1:
        case 0: return TRANSMISSION_MODE_AUTO;
    }
}

static fe_hierarchy_t GetHierarchy(void)
{
    switch ( i_hierarchy )
    {
        case 0: return HIERARCHY_NONE;
        case 1: return HIERARCHY_1;
        case 2: return HIERARCHY_2;
        case 4: return HIERARCHY_4;
        default:
            msg_Warn( NULL, "invalid intramission mode %d", i_transmission );
        case -1: return HIERARCHY_AUTO;
    }
}

/*****************************************************************************
 * FrontendInfo : Print frontend info
 *****************************************************************************/
static const char *GetFrontendTypeName( fe_type_t type )
{
    switch(type)
    {
        case FE_QPSK: return "QPSK (DVB-S/S2)";
        case FE_QAM:  return "QAM  (DVB-C)";
        case FE_OFDM: return "OFDM (DVB-T)";
        case FE_ATSC: return "ATSC";
        default: return "unknown";
    }
}

static void FrontendInfo( struct dvb_frontend_info info )
{
    msg_Dbg( NULL, "Frontend \"%s\" type \"%s\" supports:",
             info.name, GetFrontendTypeName(info.type) );
    msg_Dbg( NULL, " frequency min: %d, max: %d, stepsize: %d, tolerance: %d",
             info.frequency_min, info.frequency_max,
             info.frequency_stepsize, info.frequency_tolerance );
    msg_Dbg( NULL, " symbolrate min: %d, max: %d, tolerance: %d",
             info.symbol_rate_min, info.symbol_rate_max, info.symbol_rate_tolerance);
    msg_Dbg( NULL, " capabilities:" );

#define FRONTEND_INFO(caps,val,msg)                                             \
    if ( caps & val )                                                       \
        msg_Dbg( NULL, "  %s", msg );

    FRONTEND_INFO( info.caps, FE_IS_STUPID, "FE_IS_STUPID" )
    FRONTEND_INFO( info.caps, FE_CAN_INVERSION_AUTO, "INVERSION_AUTO" )
    FRONTEND_INFO( info.caps, FE_CAN_FEC_1_2, "FEC_1_2" )
    FRONTEND_INFO( info.caps, FE_CAN_FEC_2_3, "FEC_2_3" )
    FRONTEND_INFO( info.caps, FE_CAN_FEC_3_4, "FEC_3_4" )
    FRONTEND_INFO( info.caps, FE_CAN_FEC_4_5, "FEC_4_5" )
    FRONTEND_INFO( info.caps, FE_CAN_FEC_5_6, "FEC_5_6" )
    FRONTEND_INFO( info.caps, FE_CAN_FEC_6_7, "FEC_6_7" )
    FRONTEND_INFO( info.caps, FE_CAN_FEC_7_8, "FEC_7_8" )
    FRONTEND_INFO( info.caps, FE_CAN_FEC_8_9, "FEC_8_9" )
    FRONTEND_INFO( info.caps, FE_CAN_FEC_AUTO,"FEC_AUTO")
    FRONTEND_INFO( info.caps, FE_CAN_QPSK,   "QPSK" )
    FRONTEND_INFO( info.caps, FE_CAN_QAM_16, "QAM_16" )
    FRONTEND_INFO( info.caps, FE_CAN_QAM_32, "QAM_32" )
    FRONTEND_INFO( info.caps, FE_CAN_QAM_64, "QAM_64" )
    FRONTEND_INFO( info.caps, FE_CAN_QAM_128,"QAM_128")
    FRONTEND_INFO( info.caps, FE_CAN_QAM_256,"QAM_256")
    FRONTEND_INFO( info.caps, FE_CAN_QAM_AUTO,"QAM_AUTO" )
    FRONTEND_INFO( info.caps, FE_CAN_TRANSMISSION_MODE_AUTO, "TRANSMISSION_MODE_AUTO" )
    FRONTEND_INFO( info.caps, FE_CAN_BANDWIDTH_AUTO, "BANDWIDTH_AUTO" )
    FRONTEND_INFO( info.caps, FE_CAN_GUARD_INTERVAL_AUTO, "GUARD_INTERVAL_AUTO" )
    FRONTEND_INFO( info.caps, FE_CAN_HIERARCHY_AUTO, "HIERARCHY_AUTO" )
    FRONTEND_INFO( info.caps, FE_CAN_8VSB, "8VSB" )
    FRONTEND_INFO( info.caps, FE_CAN_16VSB,"16VSB" )
    FRONTEND_INFO( info.caps, FE_HAS_EXTENDED_CAPS, "EXTENDED_CAPS" )
#if DVBAPI_VERSION >= 501
    FRONTEND_INFO( info.caps, FE_CAN_2G_MODULATION, "2G_MODULATION" )
#endif
    FRONTEND_INFO( info.caps, FE_NEEDS_BENDING, "NEEDS_BENDING" )
    FRONTEND_INFO( info.caps, FE_CAN_RECOVER, "FE_CAN_RECOVER" )
    FRONTEND_INFO( info.caps, FE_CAN_MUTE_TS, "FE_CAN_MUTE_TS" )
#undef FRONTEND_INFO
}

/*****************************************************************************
 * FrontendSet
 *****************************************************************************/
/* S2API */
static struct dtv_property dvbs_cmdargs[] = {
    { .cmd = DTV_FREQUENCY,       .u.data = 0 },
    { .cmd = DTV_MODULATION,      .u.data = QPSK },
    { .cmd = DTV_INVERSION,       .u.data = INVERSION_AUTO },
    { .cmd = DTV_SYMBOL_RATE,     .u.data = 27500000 },
    { .cmd = DTV_INNER_FEC,       .u.data = FEC_AUTO },
    { .cmd = DTV_DELIVERY_SYSTEM, .u.data = SYS_DVBS },
    { .cmd = DTV_TUNE },
};
static struct dtv_properties dvbs_cmdseq = {
    .num = sizeof(dvbs_cmdargs)/sizeof(struct dtv_property),
    .props = dvbs_cmdargs
};

static struct dtv_property dvbs2_cmdargs[] = {
    { .cmd = DTV_FREQUENCY,       .u.data = 0 },
    { .cmd = DTV_MODULATION,      .u.data = PSK_8 },
    { .cmd = DTV_INVERSION,       .u.data = INVERSION_AUTO },
    { .cmd = DTV_SYMBOL_RATE,     .u.data = 27500000 },
    { .cmd = DTV_INNER_FEC,       .u.data = FEC_AUTO },
    { .cmd = DTV_DELIVERY_SYSTEM, .u.data = SYS_DVBS2 },
    { .cmd = DTV_PILOT,           .u.data = PILOT_AUTO },
    { .cmd = DTV_ROLLOFF,         .u.data = ROLLOFF_AUTO },
    { .cmd = DTV_TUNE },
};
static struct dtv_properties dvbs2_cmdseq = {
    .num = sizeof(dvbs2_cmdargs)/sizeof(struct dtv_property),
    .props = dvbs2_cmdargs
};

static struct dtv_property dvbc_cmdargs[] = {
    { .cmd = DTV_FREQUENCY,       .u.data = 0 },
    { .cmd = DTV_MODULATION,      .u.data = QAM_AUTO },
    { .cmd = DTV_INVERSION,       .u.data = INVERSION_AUTO },
    { .cmd = DTV_SYMBOL_RATE,     .u.data = 27500000 },
    { .cmd = DTV_DELIVERY_SYSTEM, .u.data = SYS_DVBC_ANNEX_AC },
    { .cmd = DTV_TUNE },
};
static struct dtv_properties dvbc_cmdseq = {
    .num = sizeof(dvbc_cmdargs)/sizeof(struct dtv_property),
    .props = dvbc_cmdargs
};

static struct dtv_property dvbt_cmdargs[] = {
    { .cmd = DTV_FREQUENCY,       .u.data = 0 },
    { .cmd = DTV_MODULATION,      .u.data = QAM_AUTO },
    { .cmd = DTV_INVERSION,       .u.data = INVERSION_AUTO },
    { .cmd = DTV_BANDWIDTH_HZ,    .u.data = 8000000 },
    { .cmd = DTV_CODE_RATE_HP,    .u.data = FEC_AUTO },
    { .cmd = DTV_CODE_RATE_LP,    .u.data = FEC_AUTO },
    { .cmd = DTV_GUARD_INTERVAL,  .u.data = GUARD_INTERVAL_AUTO },
    { .cmd = DTV_TRANSMISSION_MODE,.u.data = TRANSMISSION_MODE_AUTO },
    { .cmd = DTV_HIERARCHY,       .u.data = HIERARCHY_AUTO },
    { .cmd = DTV_DELIVERY_SYSTEM, .u.data = SYS_DVBT },
    { .cmd = DTV_TUNE },
};
static struct dtv_properties dvbt_cmdseq = {
    .num = sizeof(dvbt_cmdargs)/sizeof(struct dtv_property),
    .props = dvbt_cmdargs
};

#define FREQUENCY 0
#define MODULATION 1
#define INVERSION 2
#define SYMBOL_RATE 3
#define BANDWIDTH 3
#define FEC_INNER 4
#define FEC_LP 5
#define GUARD 6
#define PILOT 6
#define TRANSMISSION 7
#define ROLLOFF 7
#define HIERARCHY 8

struct dtv_property pclear[] = {
    { .cmd = DTV_CLEAR },
};

struct dtv_properties cmdclear = {
    .num = 1,
    .props = pclear
};

static void FrontendSet( bool b_init )
{
    struct dvb_frontend_info info;
    struct dtv_properties *p;

    if ( ioctl( i_frontend, FE_GET_INFO, &info ) < 0 )
    {
        msg_Err( NULL, "FE_GET_INFO failed (%s)", strerror(errno) );
        exit(1);
    }

    if ( b_init )
        FrontendInfo( info );

    /* Clear frontend commands */
    if ( ioctl( i_frontend, FE_SET_PROPERTY, &cmdclear ) < 0 )
    {
        msg_Err( NULL, "Unable to clear frontend" );
        exit(1);
    }

    switch ( info.type )
    {
    case FE_OFDM:
        p = &dvbt_cmdseq;
        p->props[FREQUENCY].u.data = i_frequency;
        p->props[INVERSION].u.data = GetInversion();
        if ( psz_modulation != NULL )
            p->props[MODULATION].u.data = GetModulation();
        p->props[BANDWIDTH].u.data = i_bandwidth * 1000000;
        p->props[FEC_INNER].u.data = GetFECInner(info.caps);
        p->props[FEC_LP].u.data = GetFECLP(info.caps);
        p->props[GUARD].u.data = GetGuard();
        p->props[TRANSMISSION].u.data = GetTransmission();
        p->props[HIERARCHY].u.data = GetHierarchy();

        msg_Dbg( NULL, "tuning OFDM frontend to f=%d bandwidth=%d inversion=%d fec_hp=%d fec_lp=%d hierarchy=%d modulation=%s guard=%d transmission=%d",
                 i_frequency, i_bandwidth, i_inversion, i_fec, i_fec_lp,
                 i_hierarchy,
                 psz_modulation == NULL ? "qam_auto" : psz_modulation,
                 i_guard, i_transmission );
        break;

    case FE_QAM:
        p = &dvbc_cmdseq;
        p->props[FREQUENCY].u.data = i_frequency;
        p->props[INVERSION].u.data = GetInversion();
        if ( psz_modulation != NULL )
            p->props[MODULATION].u.data = GetModulation();
        p->props[SYMBOL_RATE].u.data = i_srate;

        msg_Dbg( NULL, "tuning QAM frontend to f=%d srate=%d inversion=%d modulation=%s",
                 i_frequency, i_srate, i_inversion,
                 psz_modulation == NULL ? "qam_auto" : psz_modulation );
        break;

    case FE_QPSK:
        if ( psz_modulation != NULL )
        {
            p = &dvbs2_cmdseq;
            p->props[MODULATION].u.data = GetModulation();
            p->props[PILOT].u.data = GetPilot();
            p->props[ROLLOFF].u.data = GetRollOff();
        }
        else
            p = &dvbs_cmdseq;

        p->props[INVERSION].u.data = GetInversion();
        p->props[SYMBOL_RATE].u.data = i_srate;
        p->props[FEC_INNER].u.data = GetFECInner(info.caps);
        p->props[FREQUENCY].u.data = FrontendDoDiseqc();

        msg_Dbg( NULL, "tuning QPSK frontend to f=%d srate=%d inversion=%d fec=%d rolloff=%d modulation=%s pilot=%d",
                 i_frequency, i_srate, i_inversion, i_fec, i_rolloff,
                 psz_modulation == NULL ? "legacy" : psz_modulation, i_pilot );
        break;

    default:
        msg_Err( NULL, "unknown frontend type %d", info.type );
        exit(1);
    }

    /* Empty the event queue */
    for ( ; ; )
    {
        struct dvb_frontend_event event;
        if ( ioctl( i_frontend, FE_GET_EVENT, &event ) < 0
              && errno == EWOULDBLOCK )
            break;
    }

    /* Now send it all to the frontend device */
    if ( ioctl( i_frontend, FE_SET_PROPERTY, p ) < 0 )
    {
        msg_Err( NULL, "setting frontend failed (%s)", strerror(errno) );
        exit(1);
    }

    i_last_status = 0;
    i_frontend_timeout = i_wallclock + i_frontend_timeout_duration;
}

#else /* !S2API */

#warn "You are trying to compile DVBlast with an outdated linux-dvb interface."
#warn "DVBlast will be very limited and some options will have no effect."

static void FrontendSet( bool b_init )
{
    struct dvb_frontend_info info;
    struct dvb_frontend_parameters fep;

    if ( ioctl( i_frontend, FE_GET_INFO, &info ) < 0 )
    {
        msg_Err( NULL, "FE_GET_INFO failed (%s)", strerror(errno) );
        exit(1);
    }

    switch ( info.type )
    {
    case FE_OFDM:
        fep.frequency = i_frequency;
        fep.inversion = INVERSION_AUTO;

        switch ( i_bandwidth )
        {
            case 6: fep.u.ofdm.bandwidth = BANDWIDTH_6_MHZ; break;
            case 7: fep.u.ofdm.bandwidth = BANDWIDTH_7_MHZ; break;
            default:
            case 8: fep.u.ofdm.bandwidth = BANDWIDTH_8_MHZ; break;
        }

        fep.u.ofdm.code_rate_HP = FEC_AUTO;
        fep.u.ofdm.code_rate_LP = FEC_AUTO;
        fep.u.ofdm.constellation = QAM_AUTO;
        fep.u.ofdm.transmission_mode = TRANSMISSION_MODE_AUTO;
        fep.u.ofdm.guard_interval = GUARD_INTERVAL_AUTO;
        fep.u.ofdm.hierarchy_information = HIERARCHY_AUTO;

        msg_Dbg( NULL, "tuning OFDM frontend to f=%d, bandwidth=%d",
                 i_frequency, i_bandwidth );
        break;

    case FE_QAM:
        fep.frequency = i_frequency;
        fep.inversion = INVERSION_AUTO;
        fep.u.qam.symbol_rate = i_srate;
        fep.u.qam.fec_inner = FEC_AUTO;
        fep.u.qam.modulation = QAM_AUTO;

        msg_Dbg( NULL, "tuning QAM frontend to f=%d, srate=%d",
                 i_frequency, i_srate );
        break;

    case FE_QPSK:
        fep.inversion = INVERSION_AUTO;
        fep.u.qpsk.symbol_rate = i_srate;
        fep.u.qpsk.fec_inner = FEC_AUTO;
        fep.frequency = FrontendDoDiseqc();

        msg_Dbg( NULL, "tuning QPSK frontend to f=%d, srate=%d",
                 i_frequency, i_srate );
        break;

#if DVBAPI_VERSION >= 301
    case FE_ATSC:
        fep.frequency = i_frequency;

        fep.u.vsb.modulation = QAM_AUTO;

        msg_Dbg( NULL, "tuning ATSC frontend to f=%d", i_frequency );
        break;
#endif

    default:
        msg_Err( NULL, "unknown frontend type %d", info.type );
        exit(1);
    }

    /* Empty the event queue */
    for ( ; ; )
    {
        struct dvb_frontend_event event;
        if ( ioctl( i_frontend, FE_GET_EVENT, &event ) < 0
              && errno == EWOULDBLOCK )
            break;
    }

    /* Now send it all to the frontend device */
    if ( ioctl( i_frontend, FE_SET_FRONTEND, &fep ) < 0 )
    {
        msg_Err( NULL, "setting frontend failed (%s)", strerror(errno) );
        exit(1);
    }

    i_last_status = 0;
    i_frontend_timeout = i_wallclock + i_frontend_timeout_duration;
}

#endif /* S2API */

/*****************************************************************************
 * dvb_FrontendStatus
 *****************************************************************************/
uint8_t dvb_FrontendStatus( uint8_t *p_answer, ssize_t *pi_size )
{
    struct ret_frontend_status *p_ret = (struct ret_frontend_status *)p_answer;

    if ( ioctl( i_frontend, FE_GET_INFO, &p_ret->info ) < 0 )
    {
        msg_Err( NULL, "ioctl FE_GET_INFO failed (%s)", strerror(errno) );
        return RET_ERR;
    }

    if ( ioctl( i_frontend, FE_READ_STATUS, &p_ret->i_status ) < 0 )
    {
        msg_Err( NULL, "ioctl FE_READ_STATUS failed (%s)", strerror(errno) );
        return RET_ERR;
    }

    if ( p_ret->i_status & FE_HAS_LOCK )
    {
        if ( ioctl( i_frontend, FE_READ_BER, &p_ret->i_ber ) < 0 )
            msg_Err( NULL, "ioctl FE_READ_BER failed (%s)", strerror(errno) );

        if ( ioctl( i_frontend, FE_READ_SIGNAL_STRENGTH, &p_ret->i_strength )
              < 0 )
            msg_Err( NULL, "ioctl FE_READ_SIGNAL_STRENGTH failed (%s)",
                     strerror(errno) );

        if ( ioctl( i_frontend, FE_READ_SNR, &p_ret->i_snr ) < 0 )
            msg_Err( NULL, "ioctl FE_READ_SNR failed (%s)", strerror(errno) );
    }

    *pi_size = sizeof(struct ret_frontend_status);
    return RET_FRONTEND_STATUS;
}
