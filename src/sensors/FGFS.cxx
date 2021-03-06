//
// FILE: FGFS.cxx
// DESCRIPTION: aquire live sensor data from an running copy of Flightgear
//

#include <pyprops.hxx>

#include <stdlib.h>		// drand48()
#include <sys/ioctl.h>

#include <eigen3/Eigen/Core>
#include <eigen3/Eigen/Geometry>
using namespace Eigen;
#include <iostream>
using std::cout;
using std::endl;

#include "filters/nav_common/coremag.h"
#include "filters/nav_common/nav_functions_float.hxx"
#include "util/netSocket.h"
#include "util/timing.h"

#include "FGFS.hxx"


static netSocket sock_imu;
static netSocket sock_gps;

static int port_imu = 0;
static int port_gps = 0;

// property nodes
static pyPropertyNode imu_node;
static pyPropertyNode gps_node;
static pyPropertyNode airdata_node;
static pyPropertyNode act_node;
static pyPropertyNode power_node;
static pyPropertyNode config_specs_node;

static bool airdata_inited = false;

static const float D2R = M_PI / 180.0;

Vector3f mag_ned;
Quaternionf q_N2B;
Matrix3f C_N2B;

// initialize fgfs_imu input property nodes
static void bind_imu_input( pyPropertyNode *config ) {
    if ( config->hasChild("port") ) {
	port_imu = config->getLong("port");
    }
}


// initialize fgfs_gps input property nodes
static void bind_gps_input( pyPropertyNode *config ) {
    if ( config->hasChild("port") ) {
	port_gps = config->getLong("port");
    }
}


// initialize imu output property nodes 
static void bind_imu_output( string output_path ) {
    imu_node = pyGetNode(output_path, true);
    act_node = pyGetNode("/actuators", true);
    config_specs_node = pyGetNode("/config/specs", true);
    power_node = pyGetNode("/sensors/power", true);
    // set initial fake value
    power_node.setDouble( "avionics_vcc", 5.05 );
}


// initialize gps output property nodes 
static void bind_gps_output( string output_path ) {
    gps_node = pyGetNode(output_path, true);
}


// initialize airdata output property nodes 
static void bind_airdata_output( string output_path ) {
    airdata_node = pyGetNode(output_path, true);

    // set initial fake value
    airdata_node.setDouble( "temp_degC", 15.0 );

    airdata_inited = true;
}


// function prototypes
bool fgfs_imu_init( string output_path, pyPropertyNode *config ) {
    bind_imu_input( config );
    bind_imu_output( output_path );

    // open a UDP socket
    if ( ! sock_imu.open( false ) ) {
	printf("Error opening imu input socket\n");
	return false;
    }

    // bind ...
    if ( sock_imu.bind( "", port_imu ) == -1 ) {
	printf("error binding to port %d\n", port_imu );
	return false;
    }

#if 0 // we are using blocking for main loop sync now
    // don't block waiting for input
    sock_imu.setBlocking( false );
#endif
    
    return true;
}


bool fgfs_airdata_init( string output_path ) {
    bind_airdata_output( output_path );

    return true;
}


// function prototypes
bool fgfs_gps_init( string output_path, pyPropertyNode *config ) {
    bind_gps_input( config );
    bind_gps_output( output_path );

    // open a UDP socket
    if ( ! sock_gps.open( false ) ) {
	printf("Error opening imu input socket\n");
	return false;
    }

    // bind ...
    if ( sock_gps.bind( "", port_gps ) == -1 ) {
	printf("error binding to port %d\n", port_gps );
	return false;
    }

    // don't block waiting for input
    sock_gps.setBlocking( false );

    return true;
}


bool fgfs_pilot_init( string output_path, pyPropertyNode *config ) {
    return true;
}


// swap big/little endian bytes
static void my_swap( uint8_t *buf, int index, int count )
{
    int i;
    uint8_t tmp;
    for ( i = 0; i < count / 2; ++i ) {
        tmp = buf[index+i];
        buf[index+i] = buf[index+count-i-1];
        buf[index+count-i-1] = tmp;
    }
}


static bool fgfs_imu_sync_update() {
    const int fgfs_imu_size = 52;
    uint8_t packet_buf[fgfs_imu_size];

    bool fresh_data = false;

    int result;
    if ( (result = sock_imu.recv(packet_buf, fgfs_imu_size, 0))
	    == fgfs_imu_size )
    {
	fresh_data = true;

	if ( ulIsLittleEndian ) {
	    my_swap( packet_buf, 0, 8 );
	    my_swap( packet_buf, 8, 4 );
	    my_swap( packet_buf, 12, 4 );
	    my_swap( packet_buf, 16, 4 );
	    my_swap( packet_buf, 20, 4 );
	    my_swap( packet_buf, 24, 4 );
	    my_swap( packet_buf, 28, 4 );
	    my_swap( packet_buf, 32, 4 );
	    my_swap( packet_buf, 36, 4 );
	    my_swap( packet_buf, 40, 4 );
	    my_swap( packet_buf, 44, 4 );
	    my_swap( packet_buf, 48, 4 );
	}

	uint8_t *buf = packet_buf;
	/*double time = *(double *)buf;*/ buf += 8;
	float p = *(float *)buf; buf += 4;
	float q = *(float *)buf; buf += 4;
	float r = *(float *)buf; buf += 4;
	float ax = *(float *)buf; buf += 4;
	float ay = *(float *)buf; buf += 4;
	float az = *(float *)buf; buf += 4;
	float airspeed = *(float *)buf; buf += 4;
	float pressure = *(float *)buf; buf += 4;
	float roll_truth = *(float *)buf; buf += 4;
	float pitch_truth = *(float *)buf; buf += 4;
	float yaw_truth = *(float *)buf; buf += 4;

        // simulate an off kilter imu mounting
        Vector3f gv = Vector3f(p, q, r);
        Vector3f av = Vector3f(ax, ay, az);
        float a_deg = imu_node.getDouble("bank_bias_deg");
        float a_rad = a_deg * D2R;
        float sina = sin(a_rad);
        float cosa = cos(a_rad);
        Matrix3f R;
        R << 1.0,   0.0,  0.0,
             0.0, cosa,  sina,
             0.0, -sina, cosa;
        Vector3f ngv = R * gv;
        Vector3f nav = R * av;
        //cout << av << endl << nav << endl << endl;

        // generate fake magnetometer readings
        q_N2B = eul2quat(roll_truth * D2R, pitch_truth * D2R, yaw_truth * D2R);
        // rotate ideal mag vector into body frame (then normalized)
        Vector3f mag_body = q_N2B.inverse() * mag_ned;
        mag_body.normalize();
        // cout << "mag vector (body): " << mag_body(0) << " " << mag_body(1) << " " << mag_body(2) << endl;

	double cur_time = get_Time();
	imu_node.setDouble( "timestamp", cur_time );
	imu_node.setDouble( "p_rad_sec", ngv(0) );
	imu_node.setDouble( "q_rad_sec", ngv(1) );
	imu_node.setDouble( "r_rad_sec", ngv(2) );
	imu_node.setDouble( "ax_mps_sec", nav(0) );
	imu_node.setDouble( "ay_mps_sec", nav(1) );
	imu_node.setDouble( "az_mps_sec", nav(2) );
	imu_node.setDouble( "hx", mag_body(0) );
	imu_node.setDouble( "hy", mag_body(1) );
	imu_node.setDouble( "hz", mag_body(2) );
	imu_node.setDouble( "roll_truth", roll_truth );
	imu_node.setDouble( "pitch_truth", pitch_truth );
	imu_node.setDouble( "yaw_truth", yaw_truth );

	if ( airdata_inited ) {
	    airdata_node.setDouble( "timestamp", cur_time );
	    airdata_node.setDouble( "airspeed_kt", airspeed );
	    const double inhg2mbar = 33.8638866667;
	    airdata_node.setDouble( "pressure_mbar", pressure * inhg2mbar );

	    // fake volt/amp values here for no better place to do it
	    static double last_time = cur_time;
	    static double mah = 0.0;
	    double thr = act_node.getDouble("throttle");
	    power_node.setDouble("main_vcc", 16.0 - thr);
            int cells = config_specs_node.getLong("battery_cells");
            if ( cells < 1 ) { cells = 4; }
            power_node.setDouble("cell_vcc", (16.0 - thr) / cells);
	    power_node.setDouble("main_amps", thr * 12.0);
	    double dt = cur_time - last_time;
	    mah += thr*75.0 * (1000.0/3600.0) * dt;
	    last_time = cur_time;
	    power_node.setDouble( "total_mah", mah );
	}
    }

    return fresh_data;
}

// Read fgfs packets using IMU packet as the main timing reference.
// Returns the dt from the IMU perspective, not the localhost
// perspective.  This should generally be far more accurate and
// consistent.
double FGFS_update() {
    // read packets until we receive an IMU packet and the socket
    // buffer is empty.  The IMU packet (combined with being caught up
    // reading the buffer is our signal to run an interation of the
    // main loop.
    double last_time = imu_node.getDouble( "timestamp" );
    int bytes_available = 0;
    while ( true ) {
        fgfs_imu_sync_update();
	ioctl(sock_imu.getHandle(), FIONREAD, &bytes_available);
	if ( !bytes_available ) {
	    break;
        }
	// printf("looping: %d bytes available in imu sock buffer\n", bytes_available);
    }

    double cur_time = imu_node.getDouble( "timestamp" );

    return cur_time - last_time;
}


// called by imu_mgr, will always be true because the main loop sync
// actually takes care of the work and there will always be (by
// definition) fresh imu data when the imu_mgr calls this routine.
bool fgfs_imu_update() {
    return true;
}

bool fgfs_airdata_update() {
    bool fresh_data = false;

    static double last_time = 0.0;
    double cur_time = airdata_node.getDouble("timestamp");

    if ( cur_time > last_time ) {
	fresh_data = true;
    }

    last_time = cur_time;

    return fresh_data;
}


bool fgfs_gps_update() {
    const int fgfs_gps_size = 40;
    uint8_t packet_buf[fgfs_gps_size];

    bool fresh_data = false;

    int result;
    while ( (result = sock_gps.recv(packet_buf, fgfs_gps_size, 0))
	    == fgfs_gps_size )
    {
	fresh_data = true;

	if ( ulIsLittleEndian ) {
	    my_swap( packet_buf, 0, 8 );
	    my_swap( packet_buf, 8, 8 );
	    my_swap( packet_buf, 16, 8 );
	    my_swap( packet_buf, 24, 4 );
	    my_swap( packet_buf, 28, 4 );
	    my_swap( packet_buf, 32, 4 );
	    my_swap( packet_buf, 36, 4 );
	}

	uint8_t *buf = packet_buf;
	double time = *(double *)buf; buf += 8;
	double lat = *(double *)buf; buf += 8;
	double lon = *(double *)buf; buf += 8;
	float alt = *(float *)buf; buf += 4;
	float vn = *(float *)buf; buf += 4;
	float ve = *(float *)buf; buf += 4;
	float vd = *(float *)buf; buf += 4;

        if ( false ) {
            // add some random white noise
            double vel_noise = 0.1;
            double vel_offset = vel_noise * 0.5;
            vn += drand48()*vel_noise - vel_offset;
            ve += drand48()*vel_noise - vel_offset;
            vd += drand48()*vel_noise - vel_offset;
        }
        
        // compute ideal magnetic vector in ned frame
        long int jd = now_to_julian_days();
        double field[6];
        calc_magvar( lat*D2R, lon*D2R, alt / 1000.0, jd, field );
        mag_ned(0) = field[3];
        mag_ned(1) = field[4];
        mag_ned(2) = field[5];
        mag_ned.normalize();
        // cout << "mag vector (ned): " << mag_ned(0) << " " << mag_ned(1) << " " << mag_ned(2) << endl;
        
	gps_node.setDouble( "timestamp", get_Time() );
	gps_node.setDouble( "latitude_deg", lat );
	gps_node.setDouble( "longitude_deg", lon );
	gps_node.setDouble( "altitude_m", alt );
	gps_node.setDouble( "vn_ms", vn );
	gps_node.setDouble( "ve_ms", ve );
	gps_node.setDouble( "vd_ms", vd );
	gps_node.setLong( "satellites", 8 ); // fake a solid number
	gps_node.setDouble( "unix_time_sec", time );
	gps_node.setLong( "status", 2 ); // valid fix
    }

    return fresh_data;
}


bool fgfs_pilot_update() {
    return true;
}


void fgfs_imu_close() {
    sock_imu.close();
}

void fgfs_airdata_close() {
    // no op
}

void fgfs_gps_close() {
    sock_gps.close();
}

void fgfs_pilot_close() {
    // no op
}
