#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "checksum.h"
#include "include/globaldefs.h"

#include "init/globals.hxx" 	// packetizer
#include "props/props.hxx"
#include "sensors/gps_mgr.hxx"
#include "util/strutils.hxx"
#include "util/timing.h"

#include "display.h"
#include "logging.h"
#include "netBuffer.h"		// for netBuffer structure
#include "netSocket.h"
#include "packetizer.hxx"
#include "serial.hxx"

#include "remote_link.h"

using std::string;

// global variables

enum ugLinkType {
    ugUNKNOWN,
    ugUART,
    ugSOCKET
};

static SGPropertyNode *link_sequence_num = NULL;
static SGPropertyNode *link_message_time_sec = NULL;
static SGPropertyNode *link_bytes_per_frame = NULL;
bool remote_link_on = false;    // link to remote operator station
static SGSerialPort serial_fd;
static netBuffer serial_buffer(128);
static netSocket link_socket;
bool link_open = false;
static ugLinkType link_type = ugUNKNOWN;

// set up the remote link
void remote_link_init() {
    SGPropertyNode *link_type_node = fgGetNode("/config/remote-link/type", true);
    if ( strcmp(link_type_node->getString(), "uart") == 0 ) {
	if ( display_on ) {
	    printf("remote link: direct uart\n");
	}
	link_type = ugUART;
    } else if ( strcmp(link_type_node->getString(), "uart-server") == 0 ) {
	if ( display_on ) {
	    printf("remote link: via network server\n");
	}
	link_type = ugSOCKET;
    }

    if ( link_type == ugUART ) {
	SGPropertyNode *link_dev = fgGetNode("/config/remote-link/device", true);
	if ( ! serial_fd.open_port( link_dev->getString(), true ) ) {
	    return;
	}
	serial_fd.set_baud( 115200 );

	link_open = true;
    } else if ( link_type == ugSOCKET ) {
	SGPropertyNode *link_host = fgGetNode("/config/remote-link/host", true);
	SGPropertyNode *link_port = fgGetNode("/config/remote-link/port", true);
	if ( ! link_socket.open(true) ) {
	    printf("Error opening socket: %s:%d\n",
		   link_host->getString(),
		   link_port->getLong());
	    return;
	}
	if ( link_socket.connect( link_host->getString(),
				    link_port->getLong() ) < 0 )
	{
	    if ( display_on ) {
		printf("Error connecting socket: %s:%d\n",
		       link_host->getString(),
		       link_port->getLong());
	    }
	    return;
	}
	link_socket.setBlocking( false );

	link_open = true;
    }

    link_sequence_num = fgGetNode("/comms/remote-link/sequence-num", true);
    link_sequence_num->setIntValue( 0 );
    link_message_time_sec
	= fgGetNode("/comms/remote-link/last-message-sec", true);
    link_bytes_per_frame
	= fgGetNode("/config/remote-link/write-bytes-per-frame", true);
    if ( link_bytes_per_frame->getLong() == 0 ) {
	link_bytes_per_frame->setIntValue(12);
    }
}


// write as many bytes out of the serial_buffer to the uart as the
// driver will accept.
void remote_link_flush_serial() {
    if ( ! link_open || (link_type != ugUART) ) {
	// device not open, or link type is not uart
	return;
    }

    // attempt better success by writing multiple small chunks to the
    // serial port (2 * 8 = 16 bytes per call attempted)
    const int loops = 1;
    int bytes_per_frame = link_bytes_per_frame->getLong();

    for ( int i = 0; i < loops; i++ ) {
	int write_len = serial_buffer.getLength();
	if ( write_len > bytes_per_frame ) {
	    write_len = bytes_per_frame;
	}
	if ( write_len ) {
	    int bytes_written
		= serial_fd.write_port( serial_buffer.getData(),
					write_len );
	    // printf("(%d) avail = %d  written = %d\n",
	    //        i, serial_buffer.getLength(), bytes_written);
	    if ( bytes_written < 0 ) {
		// perror("serial write");
	    } else if ( bytes_written == 0 ) {
		// nothing was written
	    } else if ( bytes_written > 0 ) {
		// something was written
		serial_buffer.remove(0, bytes_written);
	    } else {
		// huh?
	    }
	}
    }
}


static short link_write( const uint8_t *buf, const short size ) {
    if ( ! link_open ) {
	// attempt to establish a socket connection if we aren't
	// connected (this could happen if the server shutdown or
	// restarted on us.
	remote_link_init();
    }

    if ( link_type == ugUART ) {
	// stuff the request in a fifo buffer and then work on writing
	// out the front end of the buffer.
	serial_buffer.append((char *)buf, size);

	remote_link_flush_serial();

	return 0; // return value not used right now
    } else if ( link_type == ugSOCKET ) {
	int result = 0;
	if ( link_open ) {
	    // ignore the SIGPIPE signal for this write (to avoid getting
	    // killed if the remote end shuts down before us
	    sighandler_t prev = signal(SIGPIPE, SIG_IGN);
	    result = link_socket.send( (const char *)buf, size );
	    signal(SIGPIPE, prev);
	    if ( result < 0 ) {
		if ( errno == EPIPE ) {
		    // remote end has shut down
		    link_open = false;
		}
	    }
	}
	return result;
    } else {
	return 0;
    }
}

static short link_read( const uint8_t *buf, const short size ) {
    if ( link_type == ugUART ) {
	return serial_fd.read_port( (char *)buf, size );
    } else if ( link_type == ugSOCKET ) {
	return link_socket.recv( (char *)buf, size );
    } else {
	return 0;
    }
}

static void gen_test_pattern( uint8_t *buf, int size ) {
    static uint8_t val = 0;

    for ( int i = 8; i < size; i++ ) {
	buf[i] = val;
    }

    val++;
}


static void remote_link_packet( const uint8_t packet_id,
				const uint8_t *packet_buf,
				const int packet_size )
{
    const int MAX_PACKET_SIZE = 256;

    // printf(" begin remote_link_packet()\n");
    uint8_t buf[MAX_PACKET_SIZE];
    uint8_t *ptr = buf;
    uint8_t cksum0, cksum1;

    // start of message sync bytes
    ptr[0] = START_OF_MSG0; ptr[1] = START_OF_MSG1;
    ptr += 2;

    // packet id (1 byte)
    ptr[0] = packet_id;
    ptr += 1;

    // packet size (1 byte)
    ptr[0] = packet_size;
    ptr += 1;

    // gen_test_pattern( (uint8_t *)packet_buf, packet_size );

    // copy packet data
    memmove( ptr, packet_buf, packet_size );
    ptr += packet_size;

    // check sum (2 bytes)
    ugear_cksum( packet_id, packet_size, packet_buf, packet_size,
		 &cksum0, &cksum1 );
    ptr[0] = cksum0; ptr[1] = cksum1;
    /*if ( packet_id == 2 ) {
	printf("cksum = %d %d\n", cksum0, cksum1);
    }*/

    link_write( buf, packet_size + 6 );
    // printf(" end remote_link_packet()\n");
}


// return a random integer between 0 and max - 1
static int my_random( int max ) {
    int result = (int)(((double)random() / RAND_MAX) * max);
    // printf("link rand(%d) = %d\n", max, result);
    return result;
}


bool remote_link_gps( uint8_t *buf, int size, int skip_count ) {
    // printf("remote link gps()\n");
    if ( skip_count < 0 ) { skip_count = 0; }
    static uint8_t skip = my_random(skip_count);

    if ( skip > 0 ) {
        --skip;
        return false;
    } else {
        skip = skip_count;
    }

    remote_link_packet( GPS_PACKET_V1, buf, size );

    return true;
}


bool remote_link_imu( uint8_t *buf, int size, int skip_count  ) {
    // printf("remote link imu()\n");
    if ( skip_count < 0 ) { skip_count = 0; }
    static uint8_t skip = my_random(skip_count);

    if ( skip > 0 ) {
        --skip;
        return false;
    } else {
        skip = skip_count;
    }

    remote_link_packet( IMU_PACKET_V2, buf, size );

    return true;
}


bool remote_link_airdata( uint8_t *buf, int size, int skip_count  ) {
    // printf("remote link airdata()\n");
    if ( skip_count < 0 ) { skip_count = 0; }
    static uint8_t skip = my_random(skip_count);

    if ( skip > 0 ) {
        --skip;
        return false;
    } else {
        skip = skip_count;
    }

    remote_link_packet( AIR_DATA_PACKET_V4, buf, size );

    return true;
}


bool remote_link_filter( uint8_t *buf, int size, int skip_count )
{
    // printf("remote link filter()\n");
    if ( skip_count < 0 ) { skip_count = 0; }
    static uint8_t skip = my_random(skip_count);

    if ( skip > 0 ) {
        --skip;
        return false;
    } else {
        skip = skip_count;
    }

    remote_link_packet( FILTER_PACKET_V1, buf, size );

    return true;
}


bool remote_link_actuator( uint8_t *buf, int size, int skip_count )
{
    // printf("remote link actuator()\n");
    if ( skip_count < 0 ) { skip_count = 0; }
    static uint8_t skip = my_random(skip_count);

    if ( skip > 0 ) {
        --skip;
        return false;
    } else {
        skip = skip_count;
    }

    remote_link_packet( ACTUATOR_PACKET_V1, buf, size );

    return true;
}


bool remote_link_pilot( uint8_t *buf, int size, int skip_count )
{
    // printf("remote link pilot()\n");
    if ( skip_count < 0 ) { skip_count = 0; }
    static uint8_t skip = my_random(skip_count);

    if ( skip > 0 ) {
        --skip;
        return false;
    } else {
        skip = skip_count;
    }

    remote_link_packet( PILOT_INPUT_PACKET_V1, buf, size );

    return true;
}


bool remote_link_ap( uint8_t *buf, int size, int skip_count )
{
    // printf("remote link ap()\n");
    if ( skip_count < 0 ) { skip_count = 0; }
    static uint8_t skip = my_random(skip_count);

    if ( skip > 0 ) {
        --skip;
        return false;
    } else {
        skip = skip_count;
    }

    remote_link_packet( AP_STATUS_PACKET_V2, buf, size );

    return true;
}


bool remote_link_health( uint8_t *buf, int size, int skip_count )
{
    // printf("remote link health()\n");
    if ( skip_count < 0 ) { skip_count = 0; }
    static uint8_t skip = my_random(skip_count);

    if ( skip > 0 ) {
        --skip;
        return false;
    } else {
        skip = skip_count;
    }

    remote_link_packet( SYSTEM_HEALTH_PACKET_V3, buf, size );

    return true;
}


bool remote_link_payload( uint8_t *buf, int size, int skip_count )
{
    // printf("remote link payload()\n");
    if ( skip_count < 0 ) { skip_count = 0; }
    static uint8_t skip = my_random(skip_count);

    if ( skip > 0 ) {
        --skip;
        return false;
    } else {
        skip = skip_count;
    }

    remote_link_packet( PAYLOAD_PACKET_V1, buf, size );

    return true;
}


static void remote_link_execute_command( const string command ) {
    vector <string> token = split( command, "," );

    if ( token.size() < 1 ) {
        // no valid tokens
        return;
    }

    // command to fly a new altitude

    // command to interrupt route and come back to some new point and
    // keep passing over it.

    if ( token[0] == "hb" && token.size() == 1 ) {
        // heart beat, no action needed
    } else if ( token[0] == "home" && token.size() == 5 ) {
        // specify new home location
        double lon = atof( token[1].c_str() );
        double lat = atof( token[2].c_str() );
        // double alt_ft = atof( token[3].c_str() );
        double azimuth_deg = atof( token[4].c_str() );

	SGPropertyNode *home_lon_node
	    = fgGetNode("/task/home/longitude-deg", true );
	SGPropertyNode *home_lat_node
	    = fgGetNode("/task/home/latitude-deg", true );
	SGPropertyNode *home_azimuth_node
	    = fgGetNode("/task/home/azimuth-deg", true );
	SGPropertyNode *home_set_node
	    = fgGetNode("/task/home/valid", true );
	home_lon_node->setDouble( lon );
	home_lat_node->setDouble( lat );
	home_azimuth_node->setDouble( azimuth_deg );
	home_set_node->setBoolValue( true );
    } else if ( token[0] == "route" && token.size() >= 5 ) {
	// find the active route manager
	if ( route_mgr != NULL ) {
	    route_mgr->clear_standby();
	    unsigned int i = 1;
	    while ( i + 4 <= token.size() ) {
		int mode = atoi( token[i].c_str() );
		double field1 = atof( token[i+1].c_str() );
		double field2 = atof( token[i+2].c_str() );
		double agl_m = -9999.9;
		if ( token[i+3] != "-" ) {
		    agl_m = atof( token[i+3].c_str() ) * SG_FEET_TO_METER;
		}
		route_mgr->new_waypoint( field1, field2, agl_m, mode );
		i += 4;
	    }
	}
    } else if ( token[0] == "route_cont" && token.size() >= 5 ) {
	// find the active route manager
	if ( route_mgr != NULL ) {
	    unsigned int i = 1;
	    while ( i + 4 <= token.size() ) {
		int mode = atoi( token[i].c_str() );
		double field1 = atof( token[i+1].c_str() );
		double field2 = atof( token[i+2].c_str() );
		double agl_m = -9999.9;
		if ( token[i+3] != "-" ) {
		    agl_m = atof( token[i+3].c_str() ) * SG_FEET_TO_METER;
		}
		route_mgr->new_waypoint( field1, field2, agl_m, mode );
		i += 4;
	    }
	}
    } else if ( token[0] == "route_end" && token.size() == 1 ) {
	// find the active route manager
	if ( route_mgr != NULL ) {
	    route_mgr->swap();
	    route_mgr->reposition();
	}
    } else if ( token[0] == "task" ) {
	SGPropertyNode *mission_command
	    = fgGetNode( "/task/command-request", true );
	mission_command->setStringValue( command.c_str() );
    } else if ( token[0] == "ap" && token.size() == 3 ) {
        // specify an autopilot target
        if ( token[1] == "agl-ft" ) {
            double agl_ft = atof( token[2].c_str() );
            SGPropertyNode *target_agl_node
                = fgGetNode( "/autopilot/settings/target-agl-ft", true );
            target_agl_node->setDouble( agl_ft );
        } else if ( token[1] == "msl-ft" ) {
            double msl_ft = atof( token[2].c_str() );
            SGPropertyNode *target_msl_node
                = fgGetNode( "/autopilot/settings/target-msl-ft", true );
            target_msl_node->setDouble( msl_ft );
        } else if ( token[1] == "speed-kt" ) {
            double speed_kt = atof( token[2].c_str() );
            SGPropertyNode *speed_kt_node
                = fgGetNode( "/autopilot/settings/target-speed-kt", true );
            speed_kt_node->setDouble( speed_kt );
        }
    } else if ( token[0] == "fcs-update" ) {
	packetizer->decode_fcs_update(token);
    } else if ( token[0] == "set" && token.size() == 3 ) {
	string prop_name = token[1];
	string value = token[2];
	SGPropertyNode *node = fgGetNode( prop_name.c_str() );
	node->setStringValue( value.c_str() );
    } else if ( token[0] == "wp" && token.size() == 5 ) {
        // specify new waypoint coordinates for a waypoint
        // int index = atoi( token[1].c_str() );
        // double lon = atof( token[2].c_str() );
        // double lat = atof( token[3].c_str() );
        // double alt_ft = atof( token[4].c_str() );
        // SGWayPoint wp( lon, lat, alt_ft * SG_FEET_TO_METER );
        // route_mgr.replace_waypoint( wp, index );
    } else if ( token[0] == "la" && token.size() == 5 ) {
	if ( token[1] == "ned" ) {
	    // set ned-vector lookat mode
	    SGPropertyNode *mode_node
                = fgGetNode( "/pointing/lookat-mode", true );
	    mode_node->setStringValue("ned-vector");
	    // specify new lookat ned coordinates
	    double north = atof( token[2].c_str() );
	    SGPropertyNode *north_node
                = fgGetNode( "/pointing/vector/north", true );
	    north_node->setDouble( north );
	    double east = atof( token[3].c_str() );
	    SGPropertyNode *east_node
                = fgGetNode( "/pointing/vector/east", true );
	    east_node->setDouble( east );
	    double down = atof( token[4].c_str() );
	    SGPropertyNode *down_node
                = fgGetNode( "/pointing/vector/down", true );
	    down_node->setDouble( down );
	} else if ( token[1] == "wgs84" ) {
	    // set wgs84 lookat mode
	    SGPropertyNode *mode_node
                = fgGetNode( "/pointing/lookat-mode", true );
	    mode_node->setStringValue("wgs84");
	    // specify new lookat ned coordinates
	    double lon = atof( token[2].c_str() );
	    SGPropertyNode *lon_node
                = fgGetNode( "/pointing/wgs84/longitude-deg", true );
	    lon_node->setDouble( lon );
	    double lat = atof( token[3].c_str() );
	    SGPropertyNode *lat_node
                = fgGetNode( "/pointing/wgs84/latitude-deg", true );
	    lat_node->setDouble( lat );
	    SGPropertyNode *ground_node
		= fgGetNode( "/position/altitude-ground-m", true );
	    double ground = ground_node->getDoubleValue();
	    SGPropertyNode *alt_node
                = fgGetNode( "/pointing/wgs84/altitude-m", true );
	    alt_node->setDouble( ground );
	}
    }
}


#define BUF_SIZE 256
static int read_link_command( char result_buf[BUF_SIZE] ) {
    // read character by character until we run out of data or find a '\n'
    // if we run out of data, save what we have so far and start with that for
    // the next call.

    // persistant data because we may not get the whole command in one call
    static char command_buf[BUF_SIZE];
    static int command_counter = 0;

    uint8_t buf[2]; buf[0] = 0;

    int result = 0;
    result = link_read( buf, 1 );
    while ( (result == 1) && (buf[0] != '\n')
	    && (command_counter < BUF_SIZE) ) {
        command_buf[command_counter] = buf[0];
        command_counter++;
        result = link_read( buf, 1 );
    }

    if ( command_counter >= BUF_SIZE ) {
	// abort this command and try again
	command_counter = 0;
    } else if ( (result == 1) && (buf[0] == '\n') ) {
        command_buf[command_counter] = 0; // terminate string
        int size = command_counter + 1;
        strncpy( result_buf, command_buf, size );
        command_counter = 0;
        return size;
    }

    return 0;
}


// calculate the nmea check sum
static char calc_nmea_cksum(const char *sentence) {
    unsigned char sum = 0;
    int i, len;

    // cout << sentence << endl;

    len = strlen(sentence);
    sum = sentence[0];
    for ( i = 1; i < len; i++ ) {
        // cout << sentence[i];
        sum ^= sentence[i];
    }
    // cout << endl;

    // printf("sum = %02x\n", sum);
    return sum;
}


// read, parse, and execute incomming commands, return true if a valid
// command received, false otherwise.
bool remote_link_command() {
    char command_buf[256];
    int result = read_link_command( command_buf );

    if ( result == 0 ) {
        return false;
    }
    
    if ( event_log_on ) {
	event_log( "remote cmd rcvd", command_buf );
    }

    string cmd = command_buf;

    // validate check sum
    if ( cmd.length() < 4 ) {
        // bogus command
        return false;
    }
    string nmea_sum = cmd.substr(cmd.length() - 2);
    cmd = cmd.substr(0, cmd.length() - 3);
    char cmd_sum[10];
    snprintf( cmd_sum, 3, "%02X", calc_nmea_cksum(cmd.c_str()) );

    if ( nmea_sum.c_str()[0] != cmd_sum[0]
         || nmea_sum.c_str()[1] != cmd_sum[1])
    {
        // checksum failure
	if ( event_log_on ) {
	    event_log( "remote cmd rcvd", "failed check sum" );
	}

        return false;
    }

    // parse the command
    string::size_type pos = cmd.find_first_of(",");
    if ( pos == string::npos ) {
        // bogus command
        return false;
    }

    // extract command sequence number
    string num = cmd.substr(0, pos);
    int sequence = atoi( num.c_str() );
    static int last_sequence_num = -1;

    // ignore repeated commands (including roll over logic)
    if ( sequence != last_sequence_num ) {

	// remainder
	cmd = cmd.substr(pos + 1);

	// execute command
	if ( event_log_on ) {
	    event_log( "remote cmd rcvd", "executed valid command" );
	}
	remote_link_execute_command( cmd );

	// register that we've received this message correctly
	link_sequence_num->setIntValue( sequence );
	last_sequence_num = sequence;
	link_message_time_sec->setDouble( get_Time() );
    }

    return true;
}
