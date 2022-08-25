/*
 * melt.c -- MLT command line utility
 * Copyright (C) 2002-2022 Meltytech, LLC
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
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <libgen.h>
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <locale.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <framework/mlt.h>

#if (defined(__APPLE__) || defined(_WIN32) || defined(HAVE_SDL2)) && !defined(MELT_NOSDL)
#include <SDL.h>
#endif

#include "io.h"
#include "JitControl.pb-c.h"
#include "JitStatus.pb-c.h"

static mlt_producer melt = NULL;
static JitStatus jit_status = JIT_STATUS__INIT;
static int jit_status_fd = -1;

static void stop_handler(int signum)
{
	if ( melt )
	{
		mlt_properties properties = MLT_PRODUCER_PROPERTIES( melt );
		mlt_properties_set_int( properties, "done", 1 );
	}
}

static void abnormal_exit_handler(int signum)
{
	// The process is going down hard. Restore the terminal first.
	term_exit();
	// Reset the default handler so the core gets dumped.
	signal( signum, SIG_DFL );
	raise( signum );
}

static void fire_jack_seek_event(mlt_properties jack, int position)
{
	mlt_events_fire(jack, "jack-seek", mlt_event_data_from_int(position));
}

static void transport_action( mlt_producer producer, char *value )
{
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( producer );
	mlt_multitrack multitrack = mlt_properties_get_data( properties, "multitrack", NULL );
	mlt_consumer consumer = mlt_properties_get_data( properties, "transport_consumer", NULL );
	mlt_properties jack = mlt_properties_get_data( MLT_CONSUMER_PROPERTIES( consumer ), "jack_filter", NULL );
	mlt_position position = producer? mlt_producer_position( producer ) : 0;

	mlt_properties_set_int( properties, "stats_off", 1 );

	JitControl *const jit_control = (JitControl*) value;
	switch (jit_control->type) {
		case CONTROL_TYPE__PAUSE:
			if (mlt_producer_get_speed( producer ) != 0) {
				mlt_producer_set_speed( producer, 0 );
				mlt_consumer_purge( consumer );
				if (jit_status.playing) {
					mlt_producer_seek( producer, mlt_consumer_position( consumer ) - 2 );
				}
			}
			mlt_events_fire( jack, "jack-stop", mlt_event_data_none() );
			jit_status.playing = 0;
			break;
		case CONTROL_TYPE__PLAY:
			if ( !jack || mlt_producer_get_speed( producer ) != 0 ) {
				mlt_producer_set_speed( producer, jit_control->play_rate );
			}
			mlt_consumer_purge( consumer );
			mlt_events_fire( jack, "jack-start", mlt_event_data_none() );
			jit_status.playing = 1;
			break;

		case CONTROL_TYPE__PLAY_RATE:
			mlt_producer_set_speed( producer, jit_control->play_rate );
			break;

		case CONTROL_TYPE__SEEK:
			mlt_consumer_purge( consumer );
			mlt_producer_seek( producer, jit_control->seek_position);
			fire_jack_seek_event(jack, jit_control->seek_position);
			break;
		case CONTROL_TYPE__QUIT:
			mlt_properties_set_int( properties, "done", 1 );
			mlt_events_fire( jack, "jack-stop", mlt_event_data_none() );
			break;
		default:
			break;
	}
	mlt_properties_set_int( MLT_CONSUMER_PROPERTIES( consumer ), "refresh", 1 );

	/*
	if ( strlen( value ) == 1 )
	{
		switch( value[ 0 ] )
		{
			case 'q':
			case 'Q':
				mlt_properties_set_int( properties, "done", 1 );
				mlt_events_fire( jack, "jack-stop", mlt_event_data_none() );
				break;
			case '0':
				position = 0;
				mlt_producer_set_speed( producer, 1 );
				mlt_producer_seek( producer, position );
				mlt_consumer_purge( consumer );
				fire_jack_seek_event(jack, position);
				break;
			case '1':
				mlt_producer_set_speed( producer, -10 );
				break;
			case '2':
				mlt_producer_set_speed( producer, -5 );
				break;
			case '3':
				mlt_producer_set_speed( producer, -2 );
				break;
			case '4':
				mlt_producer_set_speed( producer, -1 );
				break;
			case '5':
				mlt_producer_set_speed( producer, 0 );
				mlt_consumer_purge( consumer );
				mlt_producer_seek( producer, mlt_consumer_position( consumer ) + 1 );
				mlt_events_fire( jack, "jack-stop", mlt_event_data_none() );
				break;
			case '6':
			case ' ':
				if ( !jack || mlt_producer_get_speed( producer ) != 0 )
					mlt_producer_set_speed( producer, 1 );
				mlt_consumer_purge( consumer );
				mlt_events_fire( jack, "jack-start", mlt_event_data_none() );
				break;
			case '7':
				mlt_producer_set_speed( producer, 2 );
				break;
			case '8':
				mlt_producer_set_speed( producer, 5 );
				break;
			case '9':
				mlt_producer_set_speed( producer, 10 );
				break;
			case 'd':
				if ( multitrack != NULL )
				{
					int i = 0;
					mlt_position last = -1;
					fprintf( stderr, "\n" );
					for ( i = 0; 1; i ++ )
					{
						position = mlt_multitrack_clip( multitrack, mlt_whence_relative_start, i );
						if ( position == last )
							break;
						last = position;
						fprintf( stderr, "%d: %d\n", i, (int)position );
					}
				}
				break;

			case 'g':
				if ( multitrack != NULL )
				{
					position = mlt_multitrack_clip( multitrack, mlt_whence_relative_current, 0 );
					mlt_producer_seek( producer, position );
					mlt_consumer_purge( consumer );
					fire_jack_seek_event(jack, position);
				}
				break;
			case 'H':
				if ( producer != NULL )
				{
					position -= mlt_producer_get_fps( producer ) * 60;
					mlt_consumer_purge( consumer );
					mlt_producer_seek( producer, position );
					fire_jack_seek_event(jack, position);
				}
				break;
			case 'h':
				if ( producer != NULL )
				{
					position--;
					mlt_producer_set_speed( producer, 0 );
					mlt_consumer_purge( consumer );
					mlt_producer_seek( producer, position );
					mlt_events_fire( jack, "jack-stop", mlt_event_data_none() );
					fire_jack_seek_event(jack, position);
				}
				break;
			case 'j':
				if ( multitrack != NULL )
				{
					position = mlt_multitrack_clip( multitrack, mlt_whence_relative_current, 1 );
					mlt_consumer_purge( consumer );
					mlt_producer_seek( producer, position );
					fire_jack_seek_event(jack, position);
				}
				break;
			case 'k':
				if ( multitrack != NULL )
				{
					position = mlt_multitrack_clip( multitrack, mlt_whence_relative_current, -1 );
					mlt_consumer_purge( consumer );
					mlt_producer_seek( producer, position );
					fire_jack_seek_event(jack, position);
				}
				break;
			case 'l':
				if ( producer != NULL )
				{
					position++;
					mlt_consumer_purge( consumer );
					if ( mlt_producer_get_speed( producer ) != 0 )
					{
						mlt_producer_set_speed( producer, 0 );
						mlt_events_fire( jack, "jack-stop", mlt_event_data_none() );
					}
					else
					{
						mlt_producer_seek( producer, position );
						fire_jack_seek_event(jack, position);
					}
				}
				break;
			case 'L':
				if ( producer != NULL )
				{
					position += mlt_producer_get_fps( producer ) * 60;
					mlt_consumer_purge( consumer );
					mlt_producer_seek( producer, position );
					fire_jack_seek_event(jack, position);
				}
				break;
			default:
				if (producer) {
   				    mlt_position len = mlt_producer_get_length(producer);
					switch (value[0]) {
						case 'z':
							position = len;
							break;
						case 'Z':
							position = 2 * len;
							break;
						case 'x':
							position = 3 * len;
							break;
						case 'X':
							position = 4 * len;
							break;
						case 'c':
							position = 5 * len;
							break;
						case 'C':
							position = 6 * len;
							break;
						case 'v':
							position = 7 * len;
							break;
						case 'V':
							position = 8 * len;
							break;
						case 'b':
							position = 9 * len;
							break;
						case 'B':
							position = 10 * len;
							break;
						case 'n':
							position = 11 * len;
							break;
						case 'N':
							position = 12 * len;
							break;
						case 'm':
							position = 13 * len;
							break;
						case 'M':
							position = 14 * len;
							break;
						default:
							len = 0;
					}
					if (len) {
						position /= 15;
						mlt_consumer_purge( consumer );
						mlt_producer_seek( producer, position );
						fire_jack_seek_event(jack, position);
					}
				}
				break;
		}

		mlt_properties_set_int( MLT_CONSUMER_PROPERTIES( consumer ), "refresh", 1 );
	} */

	mlt_properties_set_int( properties, "stats_off", 0 );
}

static void on_jack_started( mlt_properties owner, mlt_consumer consumer, mlt_event_data event_data )
{
	mlt_producer producer = mlt_properties_get_data( MLT_CONSUMER_PROPERTIES(consumer), "transport_producer", NULL );
	if ( producer )
	{
		if ( mlt_producer_get_speed( producer ) != 0 )
		{
			mlt_properties jack = mlt_properties_get_data( MLT_CONSUMER_PROPERTIES( consumer ), "jack_filter", NULL );
			mlt_events_fire( jack, "jack-stop", mlt_event_data_none() );
		}
		else
		{
			mlt_position position = mlt_event_data_to_int(event_data);
			mlt_producer_set_speed( producer, 1 );
			mlt_consumer_purge( consumer );
			mlt_producer_seek( producer, position );
			mlt_properties_set_int( MLT_CONSUMER_PROPERTIES( consumer ), "refresh", 1 );
		}
	}
}

static void on_jack_stopped( mlt_properties owner, mlt_consumer consumer, mlt_event_data event_data )
{
	mlt_producer producer = mlt_properties_get_data( MLT_CONSUMER_PROPERTIES(consumer), "transport_producer", NULL );
	if ( producer )
	{
		mlt_position position = mlt_event_data_to_int(event_data);
		mlt_producer_set_speed( producer, 0 );
		mlt_consumer_purge( consumer );
		mlt_producer_seek( producer, position );
		mlt_properties_set_int( MLT_CONSUMER_PROPERTIES( consumer ), "refresh", 1 );
	}
}

static void setup_jack_transport( mlt_consumer consumer, mlt_profile profile )
{
	mlt_properties properties = MLT_CONSUMER_PROPERTIES( consumer );
	mlt_filter jack = mlt_factory_filter( profile, "jackrack", NULL );
	mlt_properties jack_properties = MLT_FILTER_PROPERTIES(jack);

	mlt_service_attach( MLT_CONSUMER_SERVICE(consumer), jack );
	mlt_properties_set_int( properties, "audio_off", 1 );
	mlt_properties_set_data( properties, "jack_filter", jack, 0, (mlt_destructor) mlt_filter_close, NULL );
//	mlt_properties_set( jack_properties, "out_1", "system:playback_1" );
//	mlt_properties_set( jack_properties, "out_2", "system:playback_2" );
	mlt_events_listen( jack_properties, consumer, "jack-started", (mlt_listener) on_jack_started );
	mlt_events_listen( jack_properties, consumer, "jack-stopped", (mlt_listener) on_jack_stopped );
}

static mlt_consumer create_consumer( mlt_profile profile, char *id )
{
	char *myid = id ? strdup( id ) : NULL;
	char *arg = myid ? strchr( myid, ':' ) : NULL;
	if ( arg != NULL )
		*arg ++ = '\0';
	mlt_consumer consumer = mlt_factory_consumer( profile, myid, arg );
	if ( consumer != NULL )
	{
		mlt_properties properties = MLT_CONSUMER_PROPERTIES( consumer );
		mlt_properties_set_data( properties, "transport_callback", transport_action, 0, NULL, NULL );
	}
	free( myid );
	return consumer;
}

static int load_consumer( mlt_consumer *consumer, mlt_profile profile, int argc, char **argv )
{
	int i;
	int multi = 0;
	int qglsl = 0;

	for ( i = 1; i < argc; i ++ ) {
		// See if we need multi consumer.
		multi += !strcmp( argv[i], "-consumer" );
		// Seee if we need the qglsl variant of multi consumer.
		if ( !strncmp( argv[i], "glsl.", 5 ) || !strncmp( argv[i], "movit.", 6 ) )
			qglsl = 1;
#if SDL_MAJOR_VERSION == 2
		if ( !strcmp("sdl", argv[i]) || !strcmp("sdl_audio", argv[i]) || !strcmp("sdl_preview", argv[i]) || !strcmp("sdl_still", argv[i]) ) {
			/*
			fprintf(stderr, 
"Error: This program was linked against SDL2, which is incompatible with\nSDL1 consumers. Aborting.\n");*/
			return EXIT_FAILURE;
		}
#endif
	}
	// Disable qglsl if xgl is being used!
	for ( i = 1; qglsl && i < argc; i ++ )
		if ( !strcmp( argv[i], "xgl" ) )
			qglsl = 0;

	if ( multi > 1 || qglsl )
	{
		// If there is more than one -consumer use the 'multi' consumer.
		int k = 0;
		char key[20];

		if ( *consumer )
			mlt_consumer_close( *consumer );
		*consumer = create_consumer( profile, ( qglsl? "qglsl" : "multi" ) );
		mlt_properties properties = MLT_CONSUMER_PROPERTIES( *consumer );
		for ( i = 1; i < argc; i ++ )
		{
			if ( !strcmp( argv[ i ], "-consumer" ) && argv[ i + 1 ])
			{
				// Create a properties object for each sub-consumer
				mlt_properties new_props = mlt_properties_new();
				snprintf( key, sizeof(key), "%d", k++ );
				mlt_properties_set_data( properties, key, new_props, 0,
					(mlt_destructor) mlt_properties_close, NULL );
				if ( strchr( argv[i + 1], ':' ) )
				{
					char *temp = strdup( argv[++i] );
					char *service = temp;
					char *target = strchr( temp, ':' );
					*target++ = 0;
					mlt_properties_set( new_props, "mlt_service", service );
					mlt_properties_set( new_props, "target", target );
				}
				else
				{
					mlt_properties_set( new_props, "mlt_service", argv[ ++i ] );
				}
				while ( argv[ i + 1 ] && strchr( argv[ i + 1 ], '=' ) )
					mlt_properties_parse( new_props, argv[ ++ i ] );
			}
		}
	}
	else for ( i = 1; i < argc; i ++ )
	{
		if ( !strcmp( argv[ i ], "-consumer" ) )
		{
			if ( *consumer )
				mlt_consumer_close( *consumer );
			*consumer = create_consumer( profile, argv[ ++ i ] );
			if ( *consumer )
			{
				mlt_properties properties = MLT_CONSUMER_PROPERTIES( *consumer );
				while ( argv[ i + 1 ] != NULL && strchr( argv[ i + 1 ], '=' ) )
					mlt_properties_parse( properties, argv[ ++ i ] );
			}
		}
	}
	return EXIT_SUCCESS;
}

#if defined(SDL_MAJOR_VERSION)

static void event_handling( mlt_producer producer, mlt_consumer consumer )
{
	SDL_Event event;

	while ( SDL_PollEvent( &event ) )
	{
		switch( event.type )
		{
			case SDL_QUIT:
				mlt_properties_set_int( MLT_PRODUCER_PROPERTIES( producer ), "done", 1 );
				break;

			case SDL_KEYDOWN:
#if SDL_MAJOR_VERSION == 2
				if ( event.key.keysym.sym < 0x80 && event.key.keysym.sym > 0 )
				{
					char keyboard[ 2 ] = { event.key.keysym.sym, 0 };
					transport_action( producer, keyboard );
				}
				break;

			case SDL_WINDOWEVENT:
				if ( mlt_properties_get( MLT_CONSUMER_PROPERTIES(consumer), "mlt_service" ) &&
					 !strcmp( "sdl2", mlt_properties_get( MLT_CONSUMER_PROPERTIES(consumer), "mlt_service" ) ) )
				if ( event.window.event == SDL_WINDOWEVENT_RESIZED ||
					 event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED )
				{
					mlt_properties_set_int( MLT_CONSUMER_PROPERTIES(consumer),
						"window_width", event.window.data1 );
					mlt_properties_set_int( MLT_CONSUMER_PROPERTIES(consumer),
						"window_height", event.window.data2 );
				}
				break;
#else
				if ( event.key.keysym.unicode < 0x80 && event.key.keysym.unicode > 0 )
				{
					char keyboard[ 2 ] = { event.key.keysym.unicode, 0 };
					transport_action( producer, keyboard );
				}
				break;
#endif
		}
	}
}

#endif

static JitControl *read_control() {
	static char *buf = NULL;
	static int buf_len = 0;

	int len;
	if (read(STDIN_FILENO, &len, 4) != 4) {
		exit(3);
	}
	if (buf_len < len) {
		buf = realloc(buf, len);
		if (!buf) {
			exit(4);
		}
		buf_len = len;
	}

	for (int i = 0; i < len; ) {
		const int r = read(STDIN_FILENO, buf + i, len - i);
		if (r < 1) {
			exit(5);
		}
		i += r;
	}
	return jit_control__unpack(NULL, len, buf);
}

static void write_status(JitStatus *const jit_status) {
    static char *buf = NULL;
    static int buf_len = 0;

    if (jit_status_fd < 0) {
        return;
    }

    int len = jit_status__get_packed_size(jit_status) + 4;
    if (buf_len < len) {
        buf = realloc(buf, len);
        if (!buf) {
			perror("realloc");
            exit(1);
        }
        buf_len = len;
    }

    char *b = buf;
    jit_status__pack(jit_status, b + 4);
    *((int*) b) = len - 4;
    while (len) {
        const int w = write(jit_status_fd, b, len);
        if (w < 1) {
            exit(2);
        }
        b += w;
        len -= w;
    }
}

static void transport( mlt_producer producer, mlt_consumer consumer )
{
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( producer );
	int silent = mlt_properties_get_int( MLT_CONSUMER_PROPERTIES( consumer ), "silent" );
	int progress = mlt_properties_get_int( MLT_CONSUMER_PROPERTIES( consumer ), "progress" );
	int is_getc = mlt_properties_get_int( MLT_CONSUMER_PROPERTIES( consumer ), "melt_getc" );
	struct timespec tm = { 0, 40000000 };
	int total_length = mlt_producer_get_playtime( producer );
	int last_position = 0;
    fd_set set;
    struct timeval timeout;
    int sel;

	if ( mlt_properties_get_int( properties, "done" ) == 0 && !mlt_consumer_is_stopped( consumer ) )
	{
		if ( !silent && !progress )
		{
			if ( !is_getc )
				term_init( );
/*
			fprintf( stderr, "+-----+ +-----+ +-----+ +-----+ +-----+ +-----+ +-----+ +-----+ +-----+\n" );
			fprintf( stderr, "|1=-10| |2= -5| |3= -2| |4= -1| |5=  0| |6=  1| |7=  2| |8=  5| |9= 10|\n" );
			fprintf( stderr, "+-----+ +-----+ +-----+ +-----+ +-----+ +-----+ +-----+ +-----+ +-----+\n" );

			fprintf( stderr, "+---------------------------------------------------------------------+\n" );
			fprintf( stderr, "|               H = back 1 minute,  L = forward 1 minute              |\n" );
			fprintf( stderr, "|                 h = previous frame,  l = next frame                 |\n" );
			fprintf( stderr, "|           g = start of clip, j = next clip, k = previous clip       |\n" );
			fprintf( stderr, "|                0 = restart, q = quit, space = play                  |\n" );
			fprintf( stderr, "+---------------------------------------------------------------------+\n" );
*/
		}

		while( mlt_properties_get_int( properties, "done" ) == 0 && !mlt_consumer_is_stopped( consumer ) )
		{

            //char string[2] = {0, 0};
            FD_ZERO(&set);
            FD_SET(STDIN_FILENO, &set);
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;
            if (select(STDIN_FILENO + 1, &set, NULL, NULL, &timeout) > 0) {
				/*
                    if (read(STDIN_FILENO, string, 1) != 1) {
                        string[0] = 'q';
                    }
                    if (string[0] >= '!') {
                        transport_action( producer, string );
                    }
					*/
				JitControl *const jit_control = read_control();
				if (jit_control) {
					transport_action( producer, (char*) jit_control );
					jit_control__free_unpacked(jit_control, NULL);
				}
            }

        /*
			char string[2] = {0, 0};
			int value = ( silent || progress || is_getc )? -1 : term_read( );
			if ( is_getc )
			{
				do {
					value = read(STDIN_FILENO, string, 1);
				} while (value == 1 && string[0] < '!');
				if (value == 1) {
                    value = string[0];
                } else {
                    value = 'q';
				}
			}

			if ( value != -1 )
			{
				string[0] = value;
				transport_action( producer, string );
			}
            */
            

#if defined(SDL_MAJOR_VERSION)
			event_handling( producer, consumer );
#endif

			if ( !silent && mlt_properties_get_int( properties, "stats_off" ) == 0 )
			{
                /*
				if ( progress )
				{
					int current_position = mlt_producer_position( producer );
					if ( current_position > last_position )
					{
						fprintf( stderr, "Current Frame: %10d, percentage: %10d%c",
							current_position, 100 * current_position / total_length,
							progress == 2 ? '\n' : '\r' );
						last_position = current_position;
					}
				}
				else
				{
					fprintf( stderr, "Current Position: %10d\r", (int)mlt_consumer_position( consumer ) );
				}
				fflush( stderr );
                */
                // MOFF
				jit_status.has_duration = 1;
				jit_status.duration = mlt_producer_get_length(producer);
				jit_status.has_frame_rate = 1;
				jit_status.frame_rate = mlt_producer_get_fps(producer);
				jit_status.has_play_rate = 1;
				jit_status.play_rate = mlt_producer_get_speed(producer);
				jit_status.has_position = 1;
				jit_status.position = mlt_producer_position(producer);

                write_status(&jit_status);
                last_position = jit_status.position;
			}

			//if ( silent || progress )
				//nanosleep( &tm, NULL );
		}

		//if ( !silent )
			//fprintf( stderr, "\n" );
	}
}

static void show_usage( char *program_name )
{
	fprintf( stdout,
"Usage: %s [options] [producer [name=value]* ]+\n"
"Options:\n"
"  -attach filter[:arg] [name=value]*       Attach a filter to the output\n"
"  -attach-cut filter[:arg] [name=value]*   Attach a filter to a cut\n"
"  -attach-track filter[:arg] [name=value]* Attach a filter to a track\n"
"  -attach-clip filter[:arg] [name=value]*  Attach a filter to a producer\n"
"  -audio-track | -hide-video               Add an audio-only track\n"
"  -blank frames                            Add blank silence to a track\n"
"  -chain id[:arg] [name=value]*            Add a producer as a chain\n"
"  -consumer id[:arg] [name=value]*         Set the consumer (sink)\n"
"  -debug                                   Set the logging level to debug\n"
"  -filter filter[:arg] [name=value]*       Add a filter to the current track\n"
"  -getc                                    Get keyboard input using getc\n"
"  -group [name=value]*                     Apply properties repeatedly\n"
"  -help                                    Show this message\n"
"  -jack                                    Enable JACK transport synchronization\n"
"  -join clips                              Join multiple clips into one cut\n"
"  -link id[:arg] [name=value]*             Add a link to a chain\n"
"  -mix length                              Add a mix between the last two cuts\n"
"  -mixer transition                        Add a transition to the mix\n"
"  -null-track | -hide-track                Add a hidden track\n"
"  -profile name                            Set the processing settings\n"
"  -progress                                Display progress along with position\n"
"  -query                                   List all of the registered services\n"
"  -query \"consumers\" | \"consumer\"=id       List consumers or show info about one\n"
"  -query \"filters\" | \"filter\"=id           List filters or show info about one\n"
"  -query \"producers\" | \"producer\"=id       List producers or show info about one\n"
"  -query \"transitions\" | \"transition\"=id   List transitions, show info about one\n"
"  -query \"profiles\" | \"profile\"=id         List profiles, show info about one\n"
"  -query \"presets\" | \"preset\"=id           List presets, show info about one\n"
"  -query \"formats\"                         List audio/video formats\n"
"  -query \"audio_codecs\"                    List audio codecs\n"
"  -query \"video_codecs\"                    List video codecs\n"
"  -quiet                                   Set the logging level to quiet\n"
"  -remove                                  Remove the most recent cut\n"
"  -repeat times                            Repeat the last cut\n"
"  -repository path                         Set the directory of MLT modules\n"
"  -serialise [filename]                    Write the commands to a text file\n"
"  -setlocale                               Make numeric strings locale-sensitive\n"
"  -silent                                  Do not display position/transport\n"
"  -split relative-frame                    Split the last cut into two cuts\n"
"  -swap                                    Rearrange the last two cuts\n"
"  -track                                   Add a track\n"
"  -transition id[:arg] [name=value]*       Add a transition\n"
"  -verbose                                 Set the logging level to verbose\n"
"  -timings                                 Set the logging level to timings\n"
"  -version                                 Show the version and copyright\n"
"  -video-track | -hide-audio               Add a video-only track\n"
"For more help: <https://www.mltframework.org/>\n",
	basename( program_name ) );
}

static void query_metadata( mlt_repository repo, mlt_service_type type, const char *typestr, char *id )
{
	mlt_properties metadata = mlt_repository_metadata( repo, type, id );
	if ( metadata )
	{
		char *s = mlt_properties_serialise_yaml( metadata );
		fprintf( stdout, "%s", s );
		free( s );
	}
	else
	{
		fprintf( stdout, "# No metadata for %s \"%s\"\n", typestr, id );
	}
}

static int is_service_hidden(mlt_repository repo, mlt_service_type type, const char *service_name )
{
	mlt_properties metadata = NULL;
	mlt_properties tags = NULL;
	metadata = mlt_repository_metadata(repo, type, service_name);

	if( metadata )
	{
		tags = mlt_properties_get_data( metadata, "tags", NULL );
		if( tags )
		{
			int k;
			for ( k = 0; k < mlt_properties_count( tags ); k++ )
			{
				const char* value = mlt_properties_get_value(tags, k);
				if( !strcmp("Hidden", value) )
				{
					return 1;
				}
			}
		}
	}
	return 0;
}

static void query_services( mlt_repository repo, mlt_service_type type )
{
	mlt_properties services = NULL;
	const char *typestr = NULL;
	switch ( type )
	{
		case mlt_service_consumer_type:
			services = mlt_repository_consumers( repo );
			typestr = "consumers";
			break;
		case mlt_service_filter_type:
			services = mlt_repository_filters( repo );
			typestr = "filters";
			break;
		case mlt_service_producer_type:
			services = mlt_repository_producers( repo );
			typestr = "producers";
			break;
		case mlt_service_transition_type:
			services = mlt_repository_transitions( repo );
			typestr = "transitions";
			break;
		default:
			return;
	}
	fprintf( stdout, "---\n%s:\n", typestr );
	if ( services )
	{
		int j;
		for ( j = 0; j < mlt_properties_count( services ); j++ )
		{
			const char* service_name = mlt_properties_get_name( services, j );
			if( !is_service_hidden(repo, type, service_name ) )
				fprintf( stdout, "  - %s\n", service_name );
		}
	}
	fprintf( stdout, "...\n" );
}

static void query_profiles()
{
	mlt_properties profiles = mlt_profile_list();
	fprintf( stdout, "---\nprofiles:\n" );
	if ( profiles )
	{
		int j;
		for ( j = 0; j < mlt_properties_count( profiles ); j++ )
			fprintf( stdout, "  - %s\n", mlt_properties_get_name( profiles, j ) );
	}
	fprintf( stdout, "...\n" );
	mlt_properties_close( profiles );
}

static void query_profile( const char *id )
{
	mlt_properties profiles = mlt_profile_list();
	mlt_properties profile = mlt_properties_get_data( profiles, id, NULL );
	if ( profile )
	{
		char *s = mlt_properties_serialise_yaml( profile );
		fprintf( stdout, "%s", s );
		free( s );
	}
	else
	{
		fprintf( stdout, "# No metadata for profile \"%s\"\n", id );
	}
	mlt_properties_close( profiles );
}

static void query_presets()
{
	mlt_properties presets = mlt_repository_presets();
	fprintf( stdout, "---\npresets:\n" );
	if ( presets )
	{
		int j;
		for ( j = 0; j < mlt_properties_count( presets ); j++ )
			fprintf( stdout, "  - %s\n", mlt_properties_get_name( presets, j ) );
	}
	fprintf( stdout, "...\n" );
	mlt_properties_close( presets );
}

static void query_preset( const char *id )
{
	mlt_properties presets = mlt_repository_presets();
	mlt_properties preset = mlt_properties_get_data( presets, id, NULL );
	if ( preset )
	{
		char *s = mlt_properties_serialise_yaml( preset );
		fprintf( stdout, "%s", s );
		free( s );
	}
	else
	{
		fprintf( stdout, "# No metadata for preset \"%s\"\n", id );
	}
	mlt_properties_close( presets );
}

static void query_formats( )
{
	mlt_consumer consumer = mlt_factory_consumer( NULL, "avformat", NULL );
	if ( consumer )
	{
		mlt_properties_set( MLT_CONSUMER_PROPERTIES(consumer), "f", "list" );
		mlt_consumer_start( consumer );
		mlt_consumer_close( consumer );
	}
	else
	{
		fprintf( stdout, "# No formats - failed to load avformat consumer\n" );
	}
}

static void query_acodecs( )
{
	mlt_consumer consumer = mlt_factory_consumer( NULL, "avformat", NULL );
	if ( consumer )
	{
		mlt_properties_set( MLT_CONSUMER_PROPERTIES(consumer), "acodec", "list" );
		mlt_consumer_start( consumer );
		mlt_consumer_close( consumer );
	}
	else
	{
		fprintf( stdout, "# No audio codecs - failed to load avformat consumer\n" );
	}
}

static void query_vcodecs( )
{
	mlt_consumer consumer = mlt_factory_consumer( NULL, "avformat", NULL );
	if ( consumer )
	{
		mlt_properties_set( MLT_CONSUMER_PROPERTIES(consumer), "vcodec", "list" );
		mlt_consumer_start( consumer );
		mlt_consumer_close( consumer );
	}
	else
	{
		fprintf( stdout, "# No video codecs - failed to load avformat consumer\n" );
	}
}

static void on_fatal_error( mlt_properties owner, mlt_consumer consumer )
{
	mlt_properties_set_int( MLT_CONSUMER_PROPERTIES(consumer), "done", 1 );
	mlt_properties_set_int( MLT_CONSUMER_PROPERTIES(consumer), "melt_error", 1 );
}

static void set_preview_scale(mlt_profile *profile, mlt_profile *backup_profile, double scale)
{
	*backup_profile = mlt_profile_clone(*profile);
	if (*backup_profile) {
		mlt_profile temp = *profile;
		*profile = *backup_profile;
		*backup_profile = temp;
		(*profile)->width *= scale;
		(*profile)->width -= (*profile)->width % 2;
		(*profile)->height *= scale;
		(*profile)->height -= (*profile)->height % 2;
	}
}

static mlt_repository setup_factory(const char* repo_path, int set_locale)
{
	mlt_repository repo = mlt_factory_init(repo_path);
	if (repo && set_locale) {
		// Load the system locales
		const char* locale = "";
#if defined(_WIN32)
		if (getenv("LC_ALL"))
			locale = getenv("LC_ALL");
#endif
		setlocale( LC_ALL, locale );
	}
	return repo;
}

static mlt_producer find_producer_avformat(mlt_producer p) {

	mlt_tractor tractor = (mlt_tractor) p;
	mlt_multitrack multitrack = mlt_tractor_multitrack(tractor);
	mlt_playlist playlist = (mlt_playlist) mlt_multitrack_track(multitrack, 0);
	mlt_producer clip = mlt_playlist_get_clip(playlist, 0);
	return mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(mlt_playlist_get_clip(playlist, 0)), "_cut_parent", NULL);
}

static void dump_properties(mlt_properties p) {
	for (int i = 0; i < mlt_properties_count(p); i++) {
		char *name = mlt_properties_get_name(p, i);
		printf("%s\n", name);
	}
}

int main( int argc, char **argv )
{
	int i;
	mlt_consumer consumer = NULL;
	FILE *store = NULL;
	char *name = NULL;
	mlt_profile profile = NULL;
	int is_progress = 0;
	int is_silent = 0;
	int is_abort = 0;
	int is_getc = 0;
	int error = 0;
	mlt_profile backup_profile;
	mlt_repository repo = NULL;
	const char* repo_path = NULL;
	int is_consumer_explicit = 0;
	int is_setlocale = 0;
	int status_fifo = 1;

	// Handle abnormal exit situations.
	signal( SIGSEGV, abnormal_exit_handler );
	signal( SIGILL, abnormal_exit_handler );
	signal( SIGABRT, abnormal_exit_handler );

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-setlocale")) {
			is_setlocale = 1;
			break;
		}
	}
	fprintf(stdout, "Melt starting\n");

	for ( i = 1; i < argc; i ++ )
	{
		// Check for serialisation switch
		if ( !strcmp( argv[ i ], "-serialise" ) )
		{
			name = argv[ ++ i ];
			if ( name != NULL && strstr( name, ".melt" ) )
				store = fopen( name, "w" );
			else
			{
				if ( name == NULL || name[0] == '-' )
					store = stdout;
				name = NULL;
			}
		}
		// Look for the profile option
		else if ( !strcmp( argv[ i ], "-profile" ) )
		{
			// Construct the factory
			if ( !repo )
				repo = setup_factory(repo_path, is_setlocale);

			const char *pname = argv[ ++ i ];
			if ( pname && pname[0] != '-' )
				profile = mlt_profile_init( pname );
		}
		else if ( !strcmp( argv[ i ], "-progress" ) )
		{
			is_progress = 1;
		}
		else if ( !strcmp( argv[ i ], "-progress2" ) )
		{
			is_progress = 2;
		}
		// Look for the query option
		else if ( !strcmp( argv[ i ], "-query" ) )
		{
			// Construct the factory
			if ( !repo )
				repo = setup_factory(repo_path, is_setlocale);

			const char *pname = argv[ ++ i ];
			if ( pname && pname[0] != '-' )
			{
				if ( !strcmp( pname, "consumers" ) || !strcmp( pname, "consumer" ) )
					query_services( repo, mlt_service_consumer_type );
				else if ( !strcmp( pname, "filters" ) || !strcmp( pname, "filter" ) )
					query_services( repo, mlt_service_filter_type );
				else if ( !strcmp( pname, "producers" ) || !strcmp( pname, "producer" ) )
					query_services( repo, mlt_service_producer_type );
				else if ( !strcmp( pname, "transitions" ) || !strcmp( pname, "transition" ) )
					query_services( repo, mlt_service_transition_type );
				else if ( !strcmp( pname, "profiles" ) || !strcmp( pname, "profile" ) )
					query_profiles();
				else if ( !strcmp( pname, "presets" ) || !strcmp( pname, "preset" ) )
					query_presets();
				else if ( !strncmp( pname, "format", 6 ) )
					query_formats();
				else if ( !strncmp( pname, "acodec", 6 ) || !strcmp( pname, "audio_codecs" ) )
					query_acodecs();
				else if ( !strncmp( pname, "vcodec", 6 ) || !strcmp( pname, "video_codecs" ) )
					query_vcodecs();

				else if ( !strncmp( pname, "consumer=", 9 ) )
					query_metadata( repo, mlt_service_consumer_type, "consumer", strchr( pname, '=' ) + 1 );
				else if ( !strncmp( pname, "filter=", 7 ) )
					query_metadata( repo, mlt_service_filter_type, "filter", strchr( pname, '=' ) + 1 );
				else if ( !strncmp( pname, "producer=", 9 ) )
					query_metadata( repo, mlt_service_producer_type, "producer", strchr( pname, '=' ) + 1 );
				else if ( !strncmp( pname, "transition=", 11 ) )
					query_metadata( repo, mlt_service_transition_type, "transition", strchr( pname, '=' ) + 1 );
				else if ( !strncmp( pname, "profile=", 8 ) )
					query_profile( strchr( pname, '=' ) + 1 );
				else if ( !strncmp( pname, "preset=", 7 ) )
					query_preset( strchr( pname, '=' ) + 1 );
				else
					goto query_all;
			}
			else
			{
query_all:
				query_services( repo, mlt_service_consumer_type );
				query_services( repo, mlt_service_filter_type );
				query_services( repo, mlt_service_producer_type );
				query_services( repo, mlt_service_transition_type );
				fprintf( stdout, "# You can query the metadata for a specific service using:\n"
					"# -query <type>=<identifier>\n"
					"# where <type> is one of: consumer, filter, producer, or transition.\n" );
			}
			goto exit_factory;
		}
		else if ( !strcmp( argv[ i ], "-silent" ) )
		{
			is_silent = 1;
		}
		else if ( !strcmp( argv[ i ], "-quiet" ) )
		{
			is_silent = 1;
			mlt_log_set_level( MLT_LOG_QUIET );
		}
		else if ( !strcmp( argv[ i ], "-verbose" ) )
		{
			mlt_log_set_level( MLT_LOG_VERBOSE );
		}
		else if ( !strcmp( argv[ i ], "-timings" ) )
		{
			mlt_log_set_level( MLT_LOG_TIMINGS );
		}
		else if ( !strcmp( argv[ i ], "-version" ) || !strcmp( argv[ i ], "--version" ) )
		{
			fprintf( stdout, "%s " VERSION "\n"
				"Copyright (C) 2002-2022 Meltytech, LLC\n"
				"<https://www.mltframework.org/>\n"
				"This is free software; see the source for copying conditions.  There is NO\n"
				"warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n",
				basename( argv[0] ) );
			goto exit_factory;
		}
		else if ( !strcmp( argv[ i ], "-debug" ) )
		{
			mlt_log_set_level( MLT_LOG_DEBUG );
		}
		else if ( !strcmp( argv[ i ], "-abort" ) )
		{
			is_abort = 1;
		}
		else if ( !strcmp( argv[ i ], "-getc" ) )
		{
			is_getc = 1;
		}
		else if ( !repo && !strcmp( argv[ i ], "-repository" ) )
		{
			if ( i+1 < argc && argv[i+1][0] != '-' )
				repo_path = argv[++i];
		}
		else if ( !strcmp( argv[ i ], "-consumer" ) )
		{
			is_consumer_explicit = 1;
		}
		else if ( !strcmp( argv[ i ], "-disable-status-fifo" ) )
		{
			status_fifo = 0;
		}
	}

	// Open status pipe
	if (status_fifo)
	{
		char b[100];
		sprintf(b, "/tmp/jit-status-%lld", (long long) getppid());
		fprintf(stdout, "Opening status pipe: %s\n", b);
		fflush(stdout);
		jit_status_fd = open(b, O_WRONLY);
		if (jit_status_fd < 0) {
			perror("open");
			exit(2);
		}
		fprintf(stdout, "Status pipe opened\n");
		fflush(stdout);
	}

	if ( !is_silent && !isatty( STDIN_FILENO ) && !is_progress )
		is_progress = 1;

	// Construct the factory
	if ( !repo )
		repo = setup_factory(repo_path, is_setlocale);

	// Create profile if not set explicitly
	if ( getenv( "MLT_PROFILE" ) )
		profile = mlt_profile_init( NULL );
	if ( profile == NULL )
		profile = mlt_profile_init( NULL );
	else
		profile->is_explicit = 1;

	// Look for the consumer option to load profile settings from consumer properties
	backup_profile = mlt_profile_clone( profile );
	if ( load_consumer( &consumer, profile, argc, argv ) != EXIT_SUCCESS )
		goto exit_factory;

	// If the consumer changed the profile, then it is explicit.
	if ( backup_profile && !profile->is_explicit && (
	     profile->width != backup_profile->width ||
	     profile->height != backup_profile->height ||
	     profile->sample_aspect_num != backup_profile->sample_aspect_num ||
	     profile->sample_aspect_den != backup_profile->sample_aspect_den ||
	     profile->frame_rate_den != backup_profile->frame_rate_den ||
	     profile->frame_rate_num != backup_profile->frame_rate_num ||
	     profile->colorspace != backup_profile->colorspace ) )
		profile->is_explicit = 1;
	mlt_profile_close( backup_profile );
	backup_profile = NULL;

	// Get melt producer
	if ( argc > 1 )
		melt = mlt_factory_producer( profile, "melt", &argv[ 1 ] );

	if ( melt )
	{
		// Generate an automatic profile if needed.
		if ( ! profile->is_explicit )
		{
			mlt_profile_from_producer( profile, melt );
			mlt_producer_close( melt );
			melt = mlt_factory_producer( profile, "melt", &argv[ 1 ] );
		}

		double scale = mlt_properties_get_double(MLT_CONSUMER_PROPERTIES(consumer), "scale");
		if (scale > 0.0) {
			set_preview_scale(&profile, &backup_profile, scale);
		}
		
		// Reload the consumer with the fully qualified profile.
		// The producer or auto-profile could have changed the profile.
		load_consumer( &consumer, profile, argc, argv );

		// See if producer has consumer already attached
		if ( !store && !consumer )
		{
			consumer = MLT_CONSUMER( mlt_service_consumer( MLT_PRODUCER_SERVICE( melt ) ) );
			if ( consumer )
			{
				mlt_properties_inc_ref( MLT_CONSUMER_PROPERTIES(consumer) ); // because we explicitly close it
				mlt_properties_set_data( MLT_CONSUMER_PROPERTIES(consumer),
					"transport_callback", transport_action, 0, NULL, NULL );
			}
		}

		// If we have no consumer, default to sdl
		if ( store == NULL && consumer == NULL )
			consumer = create_consumer( profile, NULL );
	}

	// video plays automatically
	jit_status.has_playing = 1;
	jit_status.playing = 1;

	// media info
	mlt_producer av = find_producer_avformat(melt);
	jit_status.media_info = calloc(1, sizeof (MediaInfo));
	media_info__init(jit_status.media_info);
	jit_status.media_info->n_streams = mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(av), "meta.media.nb_streams");
	jit_status.media_info->streams = calloc(jit_status.media_info->n_streams, sizeof (Stream*));
	for (int i = 0; i < jit_status.media_info->n_streams; i++) {
		Stream *s = calloc(sizeof (Stream), 1);
		stream__init(s);
		jit_status.media_info->streams[i] = s;
		s->has_type = 1;
		s->type = STREAM_TYPE__UNKNOWN;
		char key[100];
		sprintf(key, "meta.media.%d.stream.type", i);
		char *value = mlt_properties_get(MLT_PRODUCER_PROPERTIES(av), key);
		if (!value) {
			continue;
		} else if (!strcmp(value, "audio")) {
			s->type = STREAM_TYPE__AUDIO;
			s->audio = calloc(1, sizeof (AudioStream));
			audio_stream__init(s->audio);
			sprintf(key, "meta.media.%d.codec.channels", i);
			s->audio->has_channels = 1;
			s->audio->channels = mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(av), key);
			jit_status.has_total_channels = 1;
			jit_status.total_channels += s->audio->channels;
			sprintf(key, "meta.attr.%d.stream.language.markup", i);
			value = mlt_properties_get(MLT_PRODUCER_PROPERTIES(av), key);
			if (value) {
				//int l = strlen(value);
				//s->audio->language.len = l;
				//s->audio->language.data = calloc(1, l + 1);
				s->audio->language = strdup(value);
				//strcpy(s->audio->language.data, value);
			}
		} else if (!strcmp(value, "video")) {
			s->type = STREAM_TYPE__VIDEO;
		}
	}
	//dump_properties(av);
	//exit(1);

	// Set transport properties on consumer and produder
	if ( consumer != NULL && melt != NULL )
	{
		mlt_properties_set_data( MLT_CONSUMER_PROPERTIES( consumer ), "transport_producer", melt, 0, NULL, NULL );
		mlt_properties_set_data( MLT_PRODUCER_PROPERTIES( melt ), "transport_consumer", consumer, 0, NULL, NULL );
		if ( is_progress )
			mlt_properties_set_int(  MLT_CONSUMER_PROPERTIES( consumer ), "progress", is_progress );
		if ( is_silent )
			mlt_properties_set_int(  MLT_CONSUMER_PROPERTIES( consumer ), "silent", is_silent );
		if ( is_getc )
			mlt_properties_set_int(  MLT_CONSUMER_PROPERTIES( consumer ), "melt_getc", is_getc );
	}

	if ( argc > 1 && melt != NULL && mlt_producer_get_length( melt ) > 0 )
	{
		// Parse the arguments
		for ( i = 1; i < argc; i ++ )
		{
			if ( !strcmp( argv[ i ], "-jack" ) && consumer )
			{
				setup_jack_transport( consumer, profile );
			}
			else if ( !strcmp( argv[ i ], "-serialise" ) )
			{
				if ( store != stdout )
					i ++;
			}
			else
			{
				if ( store != NULL )
					fprintf( store, "%s\n", argv[ i ] );

				i ++;

				while ( argv[ i ] != NULL && argv[ i ][ 0 ] != '-' )
				{
					if ( store != NULL )
						fprintf( store, "%s\n", argv[ i ] );
					i += 1;
				}

				i --;
			}
		}

		if ( consumer != NULL && store == NULL )
		{
			// Get melt's properties
			mlt_properties melt_props = MLT_PRODUCER_PROPERTIES( melt );
			mlt_properties properties = MLT_CONSUMER_PROPERTIES( consumer );
	
			if (is_consumer_explicit) {
				// Apply group settings
				mlt_properties group = mlt_properties_get_data( melt_props, "group", 0 );
				mlt_properties_inherit( properties, group );
			}

			int in = mlt_properties_get_int( properties, "in" );
			int out = mlt_properties_get_int( properties, "out" );
			if ( in > 0 || out > 0 ) {
				if ( out == 0 ) {
					out = mlt_producer_get_length( melt ) - 1;
				}
				mlt_producer_set_in_and_out( melt, in, out );
				mlt_producer_seek( melt, 0 );
			}

			// smaller buffer -> faster pause
			mlt_properties_set_int( properties, "buffer", 3 );

			// Connect consumer to melt
			mlt_consumer_connect( consumer, MLT_PRODUCER_SERVICE( melt ) );

			// Start the consumer
			mlt_events_listen( properties, consumer, "consumer-fatal-error", ( mlt_listener )on_fatal_error );
			if ( mlt_consumer_start( consumer ) == 0 )
			{
				// Try to exit gracefully upon these signals
				signal( SIGINT, stop_handler );
				signal( SIGTERM, stop_handler );
#ifndef _WIN32
				signal( SIGHUP, stop_handler );
				signal( SIGPIPE, stop_handler );
#endif

				// Transport functionality
				transport( melt, consumer );
				
				// Stop the consumer
				mlt_consumer_stop( consumer );
			}	
		}
		else if ( store != NULL && store != stdout && name != NULL )
		{
			//fprintf( stderr, "Project saved as %s.\n", name );
			fclose( store );
		}
	}
	else
	{
		show_usage( argv[0] );
	}

	// Disconnect producer from consumer to prevent ref cycles from closing services
	if ( consumer )
	{
		error = mlt_properties_get_int( MLT_CONSUMER_PROPERTIES( consumer ), "melt_error" );
		mlt_consumer_connect( consumer, NULL );
		if ( !is_abort )
			mlt_events_fire( MLT_CONSUMER_PROPERTIES(consumer), "consumer-cleanup", mlt_event_data_none());
	}

	if ( is_abort )
		return error;

	// Close the producer
	if ( melt != NULL )
		mlt_producer_close( melt );

	// Close the consumer
	if ( consumer != NULL )
		mlt_consumer_close( consumer );

	// Close the factory
	mlt_profile_close( profile );
	mlt_profile_close( backup_profile );

exit_factory:

// Workaround qmelt on OS X from crashing at exit.
#if !defined(__MACH__) || !defined(QT_GUI_LIB)
	mlt_factory_close( );
#endif

	return error;
}
