//
// ZoneMinder Streaming Server, $Date$, $Revision$
// Copyright (C) 2001-2008 Philip Coombes
// 
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
// 

#include <sys/ipc.h>
#include <sys/msg.h>

#include "zm.h"
#include "zm_db.h"
#include "zm_user.h"
#include "zm_signal.h"
#include "zm_monitor.h"
#include "zm_monitorstream.h"
#include "zm_eventstream.h"

bool ValidateAccess( User *user, int mon_id ) {
  bool allowed = true;

  if ( mon_id > 0 ) {
    if ( user->getStream() < User::PERM_VIEW )
      allowed = false;
    if ( !user->canAccess( mon_id ) )
      allowed = false;
  } else {
    if ( user->getEvents() < User::PERM_VIEW )
      allowed = false;
  }
  if ( !allowed ) {
    Error( "Error, insufficient privileges for requested action user %d %s for monitor %d",
      user->Id(), user->getUsername(), mon_id );
    exit( -1 );
  }
  return( allowed );
}

int main( int argc, const char *argv[] ) {
  self = argv[0];

  srand( getpid() * time( 0 ) );

  enum { ZMS_UNKNOWN, ZMS_MONITOR, ZMS_EVENT } source = ZMS_UNKNOWN;
  enum { ZMS_JPEG, ZMS_MPEG, ZMS_RAW, ZMS_ZIP, ZMS_SINGLE } mode = ZMS_JPEG;
  char format[32] = "";
  int monitor_id = 0;
  time_t event_time = 0;
  uint64_t event_id = 0;
  unsigned int frame_id = 1;
  unsigned int scale = 100;
  unsigned int rate = 100;
  double maxfps = 10.0;
  unsigned int bitrate = 100000;
  unsigned int ttl = 0;
  EventStream::StreamMode replay = EventStream::MODE_SINGLE;
  std::string username;
  std::string password;
  char auth[64] = "";
  unsigned int connkey = 0;
  unsigned int playback_buffer = 0;

  bool nph = false;
  const char *basename = strrchr( argv[0], '/' );
  if (basename) //if we found a / lets skip past it
    basename++;
  else //argv[0] will not always contain the full path, but rather just the script name
    basename = argv[0];
  const char *nph_prefix = "nph-";
  if ( basename && !strncmp( basename, nph_prefix, strlen(nph_prefix) ) ) {
    nph = true;
  }
  
  zmLoadConfig();


  const char *query = getenv( "QUERY_STRING" );
  if ( query ) {
    Debug( 1, "Query: %s", query );
  
    char temp_query[1024];
    strncpy( temp_query, query, sizeof(temp_query) );
    char *q_ptr = temp_query;
    char *parms[16]; // Shouldn't be more than this
    int parm_no = 0;
    while( (parm_no < 16) && (parms[parm_no] = strtok( q_ptr, "&" )) ) {
      parm_no++;
      q_ptr = NULL;
    }
  
    for ( int p = 0; p < parm_no; p++ ) {
      char *name = strtok( parms[p], "=" );
      char *value = strtok( NULL, "=" );
      if ( !value )
        value = (char *)"";
      if ( !strcmp( name, "source" ) ) {
        source = !strcmp( value, "event" )?ZMS_EVENT:ZMS_MONITOR;
      } else if ( !strcmp( name, "mode" ) ) {
        mode = !strcmp( value, "jpeg" )?ZMS_JPEG:ZMS_MPEG;
        mode = !strcmp( value, "raw" )?ZMS_RAW:mode;
        mode = !strcmp( value, "zip" )?ZMS_ZIP:mode;
        mode = !strcmp( value, "single" )?ZMS_SINGLE:mode;
      } else if ( !strcmp( name, "format" ) ) {
        strncpy( format, value, sizeof(format) );
      } else if ( !strcmp( name, "monitor" ) ) {
        monitor_id = atoi( value );
        if ( source == ZMS_UNKNOWN )
          source = ZMS_MONITOR;
      } else if ( !strcmp( name, "time" ) ) {
        event_time = atoi( value );
      } else if ( !strcmp( name, "event" ) ) {
        event_id = strtoull( value, (char **)NULL, 10 );
        source = ZMS_EVENT;
      } else if ( !strcmp( name, "frame" ) ) {
        frame_id = strtoull( value, (char **)NULL, 10 );
        source = ZMS_EVENT;
      } else if ( !strcmp( name, "scale" ) ) {
        scale = atoi( value );
      } else if ( !strcmp( name, "rate" ) ) {
        rate = atoi( value );
      } else if ( !strcmp( name, "maxfps" ) ) {
        maxfps = atof( value );
      } else if ( !strcmp( name, "bitrate" ) ) {
        bitrate = atoi( value );
      } else if ( !strcmp( name, "ttl" ) ) {
        ttl = atoi(value);
      } else if ( !strcmp( name, "replay" ) ) {
        replay = !strcmp( value, "gapless" )?EventStream::MODE_ALL_GAPLESS:EventStream::MODE_SINGLE;
        replay = !strcmp( value, "all" )?EventStream::MODE_ALL:replay;
      } else if ( !strcmp( name, "connkey" ) ) {
        connkey = atoi(value);
      } else if ( !strcmp( name, "buffer" ) ) {
        playback_buffer = atoi(value);
      } else if ( config.opt_use_auth ) {
        if ( strcmp( config.auth_relay, "none" ) == 0 ) {
          if ( !strcmp( name, "user" ) ) {
            username = value;
          }
        } else {
          //if ( strcmp( config.auth_relay, "hashed" ) == 0 )
          {
            if ( !strcmp( name, "auth" ) ) {
              strncpy( auth, value, sizeof(auth)-1 );
            }
          }
          //else if ( strcmp( config.auth_relay, "plain" ) == 0 )
          {
            if ( !strcmp( name, "user" ) ) {
              username = UriDecode( value );
            }
            if ( !strcmp( name, "pass" ) ) {
              password = UriDecode( value );
              Debug( 1, "Have %s for password", password.c_str() );
            }
          }
        }
      }
    } // end foreach parm
  } // end if query

  char log_id_string[32] = "zms";
  if ( monitor_id ) {
    snprintf(log_id_string, sizeof(log_id_string), "zms_m%d", monitor_id);
  } else {
    snprintf(log_id_string, sizeof(log_id_string), "zms_e%" PRIu64, event_id);
  }
  logInit( log_id_string );

  if ( config.opt_use_auth ) {
    User *user = 0;

    if ( strcmp( config.auth_relay, "none" ) == 0 ) {
      if ( username.length() ) {
        user = zmLoadUser( username.c_str() );
      }
    } else {
      //if ( strcmp( config.auth_relay, "hashed" ) == 0 )
      {
        if ( *auth ) {
          user = zmLoadAuthUser( auth, config.auth_hash_ips );
        }
      }
      //else if ( strcmp( config.auth_relay, "plain" ) == 0 )
      {
        if ( username.length() && password.length() ) {
          user = zmLoadUser( username.c_str(), password.c_str() );
        }
      }
    }
    if ( !user ) {
      Error( "Unable to authenticate user" );
      logTerm();
      zmDbClose();
      return( -1 );
    }
    ValidateAccess( user, monitor_id );
  }

  hwcaps_detect();
  zmSetDefaultTermHandler();
  zmSetDefaultDieHandler();

  setbuf( stdout, 0 );
  if ( nph ) {
    fprintf( stdout, "HTTP/1.0 200 OK\r\n" );
  }
  fprintf( stdout, "Server: ZoneMinder Video Server/%s\r\n", ZM_VERSION );
        
  time_t now = time( 0 );
  char date_string[64];
  strftime( date_string, sizeof(date_string)-1, "%a, %d %b %Y %H:%M:%S GMT", gmtime( &now ) );

  fprintf( stdout, "Expires: Mon, 26 Jul 1997 05:00:00 GMT\r\n" );
  fprintf( stdout, "Last-Modified: %s\r\n", date_string );
  fprintf( stdout, "Cache-Control: no-store, no-cache, must-revalidate\r\n" );
  fprintf( stdout, "Cache-Control: post-check=0, pre-check=0\r\n" );
  fprintf( stdout, "Pragma: no-cache\r\n");
  // Removed as causing more problems than it fixed.
  //if ( !nph )
  //{
    //fprintf( stdout, "Content-Length: 0\r\n");
  //}

  if ( source == ZMS_MONITOR ) {
    MonitorStream stream;
    stream.setStreamScale( scale );
    stream.setStreamReplayRate( rate );
    stream.setStreamMaxFPS( maxfps );
    stream.setStreamTTL( ttl );
    stream.setStreamQueue( connkey );
    stream.setStreamBuffer( playback_buffer );
    if ( ! stream.setStreamStart( monitor_id ) ) {
      Error( "Unable to connect to zmc process for monitor %d", monitor_id );
      fprintf( stderr, "Unable to connect to zmc process.  Please ensure that it is running." );
      logTerm();
      zmDbClose();
      return( -1 );
    } 

    if ( mode == ZMS_JPEG ) {
      stream.setStreamType( MonitorStream::STREAM_JPEG );
    } else if ( mode == ZMS_RAW ) {
      stream.setStreamType( MonitorStream::STREAM_RAW );
    } else if ( mode == ZMS_ZIP ) {
      stream.setStreamType( MonitorStream::STREAM_ZIP );
    } else if ( mode == ZMS_SINGLE ) {
      stream.setStreamType( MonitorStream::STREAM_SINGLE );
    } else {
#if HAVE_LIBAVCODEC
      stream.setStreamFormat( format );
      stream.setStreamBitrate( bitrate );
      stream.setStreamType( MonitorStream::STREAM_MPEG );
#else // HAVE_LIBAVCODEC
      Error( "MPEG streaming of '%s' attempted while disabled", query );
      fprintf( stderr, "MPEG streaming is disabled.\nYou should configure with the --with-ffmpeg option and rebuild to use this functionality.\n" );
      logTerm();
      zmDbClose();
      return( -1 );
#endif // HAVE_LIBAVCODEC
    }
    stream.runStream();
  } else if ( source == ZMS_EVENT ) {
    if ( ! event_id ) {
      Fatal( "Can't view an event without specifying an event_id." );
    }
    Debug(3,"Doing event stream scale(%d)", scale );
    EventStream stream;
    stream.setStreamScale( scale );
    stream.setStreamReplayRate( rate );
    stream.setStreamMaxFPS( maxfps );
    stream.setStreamMode( replay );
    stream.setStreamQueue( connkey );
    if ( monitor_id && event_time ) {
      stream.setStreamStart(monitor_id, event_time);
    } else {
      Debug(3, "Setting stream start to frame (%d)", frame_id);
      stream.setStreamStart(event_id, frame_id);
    }
    if ( mode == ZMS_JPEG ) {
      stream.setStreamType( EventStream::STREAM_JPEG );
    } else {
#if HAVE_LIBAVCODEC
      stream.setStreamFormat( format );
      stream.setStreamBitrate( bitrate );
      stream.setStreamType( EventStream::STREAM_MPEG );
#else // HAVE_LIBAVCODEC
      Error( "MPEG streaming of '%s' attempted while disabled", query );
      fprintf( stderr, "MPEG streaming is disabled.\nYou should ensure the ffmpeg libraries are installed and detected and rebuild to use this functionality.\n" );
      logTerm();
      zmDbClose();
      return( -1 );
#endif // HAVE_LIBAVCODEC
    } // end if jpeg or mpeg
    stream.runStream();
  } else {
    Error("Neither a monitor or event was specified.");
  } // end if monitor or event

  logTerm();
  zmDbClose();

  return( 0 );
}
