/*
 * filter_jackrack.c -- filter audio through Jack and/or LADSPA plugins
 * Copyright (C) 2004 Ushodaya Enterprises Limited
 * Author: Dan Dennedy <dan@dennedy.org>
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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "filter_jackrack.h"

#include <framework/mlt_frame.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <pthread.h>
#include <string.h>

#include "ui.h"

#define BUFFER_LEN 2048 * 20

static void *jackrack_thread( void *arg )
{
	mlt_properties properties = arg;
	ui_t *jackrack = mlt_properties_get_data( properties, "jackrack", NULL );
		
	while ( mlt_properties_get_int( properties, "_done" ) == 0 )
		if ( ui_loop_iterate( jackrack ) )
			break;
		
	ui_quit( jackrack );
	ui_destroy( jackrack );
	
	return NULL;
}

static void initialise_jack_ports( mlt_properties properties, int channels, int samples )
{
	int i;
	char mlt_name[20], rack_name[30];
	jack_port_t **port = NULL;
	jack_client_t *jack_client = mlt_properties_get_data( properties, "jack_client", NULL );
	jack_nframes_t jack_buffer_size = jack_get_buffer_size( jack_client );
	
	// Propogate these for the Jack processing callback
	mlt_properties_set_int( properties, "_channels", channels );
	mlt_properties_set_int( properties, "_samples", samples );

	// Start JackRack
	if ( mlt_properties_get( properties, "src" ) )
	{
		pthread_t *jackrack_pthread = mlt_pool_alloc( sizeof( pthread_t ) );

		snprintf( rack_name, sizeof( rack_name ), "jackrack%d", getpid() );
		ui_t *jackrack = ui_new( rack_name, mlt_properties_get_int( properties, "_channels" ), 0, 0 );
		jack_rack_open_file( jackrack, mlt_properties_get( properties, "src" ) );		
		
		mlt_properties_set_data( properties, "jackrack", jackrack, 0, NULL, NULL );
		mlt_properties_set( properties, "_rack_client_name", rack_name );
		mlt_properties_set_int( properties, "_done", 0 );
		mlt_properties_set_data( properties, "jackrack_pthread", jackrack_pthread, 0, NULL, NULL );
		
		pthread_create( jackrack_pthread, NULL, jackrack_thread, properties );
	}
		
	// Allocate buffers and ports
	jack_ringbuffer_t **output_buffers = mlt_pool_alloc( sizeof( jack_ringbuffer_t *) * channels );
	jack_ringbuffer_t **input_buffers = mlt_pool_alloc( sizeof( jack_ringbuffer_t *) * channels );
	jack_port_t **jack_output_ports = mlt_pool_alloc( sizeof(jack_port_t *) * channels );
	jack_port_t **jack_input_ports = mlt_pool_alloc( sizeof(jack_port_t *) * channels );
	float **jack_output_buffers = mlt_pool_alloc( sizeof(float *) * jack_buffer_size );
	float **jack_input_buffers = mlt_pool_alloc( sizeof(float *) * jack_buffer_size );

	// Set properties for self-destruction	
	mlt_properties_set_data( properties, "output_buffers", output_buffers, sizeof( jack_ringbuffer_t *) * channels, NULL, NULL );
	mlt_properties_set_data( properties, "input_buffers", input_buffers, sizeof( jack_ringbuffer_t *) * channels, NULL, NULL );
	mlt_properties_set_data( properties, "jack_output_ports", jack_output_ports, sizeof( jack_port_t *) * channels, NULL, NULL );
	mlt_properties_set_data( properties, "jack_input_ports", jack_input_ports, sizeof( jack_port_t *) * channels, NULL, NULL );
	mlt_properties_set_data( properties, "jack_output_buffers", jack_output_buffers, sizeof( float *) * channels, NULL, NULL );
	mlt_properties_set_data( properties, "jack_input_buffers", jack_input_buffers, sizeof( float *) * channels, NULL, NULL );

	// Start Jack processing - required before registering ports
	jack_activate( jack_client );
	
	// Register Jack ports
	for ( i = 0; i < channels; i++ )
	{
		int in;
		
		output_buffers[i] = jack_ringbuffer_create( BUFFER_LEN * sizeof(float) );
		input_buffers[i] = jack_ringbuffer_create( BUFFER_LEN * sizeof(float) );
		snprintf( mlt_name, sizeof( mlt_name ), "obuf%d", i );
		mlt_properties_set_data( properties, mlt_name, output_buffers[i], BUFFER_LEN * sizeof(float), NULL, NULL );
		snprintf( mlt_name, sizeof( mlt_name ), "ibuf%d", i );
		mlt_properties_set_data( properties, mlt_name, input_buffers[i], BUFFER_LEN * sizeof(float), NULL, NULL );
		
		for ( in = 0; in < 2; in++ )
		{
			snprintf( mlt_name, sizeof( mlt_name ), "%s_%d", in ? "in" : "out", i + 1);
			port = ( in ? &jack_input_ports[i] : &jack_output_ports[i] );
			
			*port =  jack_port_register( jack_client, mlt_name, JACK_DEFAULT_AUDIO_TYPE,
				( in ? JackPortIsInput : JackPortIsOutput ) | JackPortIsTerminal, 0 );
		}
	}
	
	// Establish connections
	for ( i = 0; i < channels; i++ )
	{
		int in;
		for ( in = 0; in < 2; in++ )
		{
			port = ( in ? &jack_input_ports[i] : &jack_output_ports[i] );
			snprintf( mlt_name, sizeof( mlt_name ), "%s", jack_port_name( *port ) );

			snprintf( rack_name, sizeof( rack_name ), "%s_%d", in ? "in" : "out", i + 1 );
			if ( mlt_properties_get( properties, "_rack_client_name" ) )
				snprintf( rack_name, sizeof( rack_name ), "%s:%s_%d", mlt_properties_get( properties, "_rack_client_name" ), in ? "out" : "in", i + 1);
			else if ( mlt_properties_get( properties, rack_name ) )
				snprintf( rack_name, sizeof( rack_name ), "%s", mlt_properties_get( properties, rack_name ) );
			else
				snprintf( rack_name, sizeof( rack_name ), "%s:%s_%d", mlt_properties_get( properties, "_client_name" ), in ? "out" : "in", i + 1);
			
			if ( in )
			{
				fprintf( stderr, "jack connect %s to %s\n", rack_name, mlt_name );
				jack_connect( jack_client, rack_name, mlt_name );
			}
			else
			{
				fprintf( stderr, "jack connect %s to %s\n", mlt_name, rack_name );
				jack_connect( jack_client, mlt_name, rack_name );
			}
		}
	}
}

static int jack_process (jack_nframes_t frames, void * data)
{
	mlt_filter filter = (mlt_filter) data;
 	mlt_properties properties = MLT_FILTER_PROPERTIES( filter );
	int channels = mlt_properties_get_int( properties, "_channels" );
	int err = 0;
	int i;
	static size_t total_size = 0;
	size_t first_ring_size = mlt_properties_get_int( properties, "_samples" ) * sizeof(float);
  
	jack_ringbuffer_t **output_buffers = mlt_properties_get_data( properties, "output_buffers", NULL );
	if ( output_buffers == NULL )
		return 0;
	jack_ringbuffer_t **input_buffers = mlt_properties_get_data( properties, "input_buffers", NULL );
	jack_port_t **jack_output_ports = mlt_properties_get_data( properties, "jack_output_ports", NULL );
	jack_port_t **jack_input_ports = mlt_properties_get_data( properties, "jack_input_ports", NULL );
	float **jack_output_buffers = mlt_properties_get_data( properties, "jack_output_buffers", NULL );
	float **jack_input_buffers = mlt_properties_get_data( properties, "jack_input_buffers", NULL );
	pthread_mutex_t *output_lock = mlt_properties_get_data( properties, "output_lock", NULL );
	pthread_cond_t *output_ready = mlt_properties_get_data( properties, "output_ready", NULL );
	
	for ( i = 0; i < channels; i++ )
	{
		size_t jack_size = ( frames * sizeof(float) );
		size_t ring_size;
		
		// Send audio through out port
		jack_output_buffers[i] = jack_port_get_buffer( jack_output_ports[i], frames );
		if ( ! jack_output_buffers[i] )
		{
			fprintf( stderr, "%s: no jack buffer for output port %d\n", __FUNCTION__, i );
			err = 1;
			break;
		}
		ring_size = jack_ringbuffer_read_space( output_buffers[i] );
		jack_ringbuffer_read( output_buffers[i], ( char * )jack_output_buffers[i], ring_size < jack_size ? ring_size : jack_size );
		
		// Do not start returning audio until we have sent first mlt frame
		if ( first_ring_size != -sizeof(float) && i == 0 )
			total_size += ring_size;
		if ( first_ring_size == -sizeof(float) || total_size >= first_ring_size )
		{
			// Return audio through in port
			jack_input_buffers[i] = jack_port_get_buffer( jack_input_ports[i], frames );
			if ( ! jack_input_buffers[i] )
			{
				fprintf( stderr, "%s: no jack buffer for input port %d\n", __FUNCTION__, i );
				err = 1;
				break;
			}
			
			ring_size = jack_ringbuffer_write_space( input_buffers[i] );
			jack_ringbuffer_write( input_buffers[i], ( char * )jack_input_buffers[i], ring_size < jack_size ? ring_size : jack_size );
			
			// Tell mlt that audio is available
			if ( first_ring_size != -sizeof(float) && i == ( channels - 1 ) 
				 && pthread_mutex_trylock( output_lock) == 0 )
			{
				pthread_cond_signal( output_ready );
				pthread_mutex_unlock( output_lock );
			}
			
			// Set flag to skip this henceforth
			mlt_properties_set_int( properties, "_samples", -1 );
		}
	}

	return err;
}


/** Get the audio.
*/

static int jackrack_get_audio( mlt_frame frame, int16_t **buffer, mlt_audio_format *format, int *frequency, int *channels, int *samples )
{
	// Get the filter service
	mlt_filter filter = mlt_frame_pop_audio( frame );

	// Get the filter properties
	mlt_properties filter_properties = MLT_FILTER_PROPERTIES( filter );

	// Restore the original get_audio
	frame->get_audio = mlt_frame_pop_audio( frame );
	
	int jack_frequency = mlt_properties_get_int( filter_properties, "_sample_rate" );

	// Get the producer's audio
	mlt_frame_get_audio( frame, buffer, format, &jack_frequency, channels, samples );
	//fprintf( stderr, "%s: %d frames %d channels\n", __FUNCTION__, *samples, *channels );
	
	// Deal with sample rate differences
	if ( *frequency != jack_frequency )
		fprintf( stderr, "mismatching frequencies in filter jackrack\n" );
	*frequency = jack_frequency;

	// Initialise Jack ports and connections if needed
	if ( ! mlt_properties_get_data( filter_properties, "jack_output_ports", NULL ) )
		initialise_jack_ports( filter_properties, *channels, *samples );
	
	// Get the filter-specific properties
	jack_ringbuffer_t **output_buffers = mlt_properties_get_data( filter_properties, "output_buffers", NULL );
	jack_ringbuffer_t **input_buffers = mlt_properties_get_data( filter_properties, "input_buffers", NULL );
	pthread_mutex_t *output_lock = mlt_properties_get_data( filter_properties, "output_lock", NULL );
	pthread_cond_t *output_ready = mlt_properties_get_data( filter_properties, "output_ready", NULL );
	
	// Process the audio
	int16_t *q = *buffer;
	float sample;
	int i, j;

	// Convert to floats and write into output ringbuffer
	if ( jack_ringbuffer_write_space( output_buffers[0] ) >= ( *samples * sizeof(float) ) )
	{
		//fprintf( stderr, "%s: buffer overrun!\n", __FUNCTION__ );
		//pthread_cond_wait( &output_ready, &output_lock );
		for ( i = 0; i < *samples; i++ )
			for ( j = 0; j < *channels; j++ )
			{
				sample = ( float )( *q ++ ) / 32768.0;
				jack_ringbuffer_write( output_buffers[j], ( char * )&sample, sizeof(float) );
			}
	}
	//else
	//	fprintf( stderr, "%s: out buffer size %d\n", __FUNCTION__, jack_ringbuffer_write_space( output_buffers[0] ) );
	
	// Read from input ringbuffer and convert from floats
	while ( mlt_properties_get_int( filter_properties, "_samples" ) != -1
		    && jack_ringbuffer_read_space( input_buffers[ *channels - 1 ] ) < ( *samples * sizeof(float) ) )
		pthread_cond_wait( output_ready, output_lock );
		
	// Initialise to silence, but repeat last frame if available in case of 
	// buffer underrun
	sample = 0;
	q = *buffer;
	for ( i = 0; i < *samples; i++ )
		for ( j = 0; j < *channels; j++ )
		{
			jack_ringbuffer_read( input_buffers[j], ( char * )&sample, sizeof(float) );

			if ( sample > 1.0 )
				sample = 1.0;
			else if ( sample < -1.0 )
				sample = -1.0;
		
			if ( sample > 0 )
				*q ++ = 32767 * sample;
			else
				*q ++ = 32768 * sample;
		}

	return 0;
}


/** Filter processing.
*/

static mlt_frame filter_process( mlt_filter this, mlt_frame frame )
{
	if ( frame->get_audio != NULL )
	{
		mlt_frame_push_audio( frame, frame->get_audio );
		mlt_frame_push_audio( frame, this );
		frame->get_audio = jackrack_get_audio;
	}

	return frame;
}


void filter_close( mlt_filter this )
{
	int i;
	char mlt_name[20];
	mlt_properties properties = MLT_FILTER_PROPERTIES( this );
	jack_client_t *jack_client = mlt_properties_get_data( properties, "jack_client", NULL );
	
	jack_deactivate( jack_client );
	jack_client_close( jack_client );
	for ( i = 0; i < mlt_properties_get_int( properties, "_channels" ); i++ )
	{
		snprintf( mlt_name, sizeof( mlt_name ), "obuf%d", i );
		jack_ringbuffer_free( mlt_properties_get_data( properties, mlt_name, NULL ) );
		snprintf( mlt_name, sizeof( mlt_name ), "ibuf%d", i );
		jack_ringbuffer_free( mlt_properties_get_data( properties, mlt_name, NULL ) );
	}
	mlt_pool_release( mlt_properties_get_data( properties, "output_buffers", NULL ) );
	mlt_pool_release( mlt_properties_get_data( properties, "input_buffers", NULL ) );
	mlt_pool_release( mlt_properties_get_data( properties, "jack_output_ports", NULL ) );
	mlt_pool_release( mlt_properties_get_data( properties, "jack_input_ports", NULL ) );
	mlt_pool_release( mlt_properties_get_data( properties, "jack_output_buffers", NULL ) );
	mlt_pool_release( mlt_properties_get_data( properties, "jack_input_buffers", NULL ) );
	mlt_pool_release( mlt_properties_get_data( properties, "output_lock", NULL ) );
	mlt_pool_release( mlt_properties_get_data( properties, "output_ready", NULL ) );
	
	pthread_t *jackrack_pthread = mlt_properties_get_data( properties, "jackrack_thread", NULL );
	if ( jackrack_pthread != NULL )
	{
		mlt_properties_set_int( properties, "_done", 1 );
		pthread_join( *jackrack_pthread, NULL );
		mlt_pool_release( jackrack_pthread );
	}
	
	this->parent.close = NULL;
	mlt_service_close( &this->parent );
}

/** Constructor for the filter.
*/

mlt_filter filter_jackrack_init( char *arg )
{
	mlt_filter this = mlt_filter_new( );
	if ( this != NULL )
	{
		char name[14];
		
		snprintf( name, sizeof( name ), "mlt%d", getpid() );
		jack_client_t *jack_client = jack_client_new( name );
		if ( jack_client )
		{
			mlt_properties properties = MLT_FILTER_PROPERTIES( this );
			pthread_mutex_t *output_lock = mlt_pool_alloc( sizeof( pthread_mutex_t ) );
			pthread_cond_t  *output_ready = mlt_pool_alloc( sizeof( pthread_cond_t ) );
			
			jack_set_process_callback( jack_client, jack_process, this );
			//TODO: jack_on_shutdown( jack_client, jack_shutdown_cb, this );
			this->process = filter_process;
			this->close = filter_close;
			pthread_mutex_init( output_lock, NULL );
			pthread_cond_init( output_ready, NULL );
			
			mlt_properties_set( properties, "src", arg );
			mlt_properties_set( properties, "_client_name", name );
			mlt_properties_set_data( properties, "jack_client", jack_client, 0, NULL, NULL );
			mlt_properties_set_int( properties, "_sample_rate", jack_get_sample_rate( jack_client ) );
			mlt_properties_set_data( properties, "output_lock", output_lock, 0, NULL, NULL );
			mlt_properties_set_data( properties, "output_ready", output_ready, 0, NULL, NULL );
		}
	}
	return this;
}
