//
// imu_mgr.hxx - front end IMU sensor management interface
//
// Written by Curtis Olson, curtolson <at> gmail <dot> com.  Spring 2009.
// This code is released into the public domain.
// 

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "comms/logging.h"
#include "comms/remote_link.h"
#include "comms/packetizer.hxx"
#include "include/globaldefs.h"
#include "init/globals.hxx"
#include "python/pyprops.hxx"
#include "util/myprof.h"
#include "util/timing.h"

#include "sensors/APM2.hxx"
#include "sensors/Goldy2.hxx"
#include "sensors/imu_fgfs.hxx"
#include "sensors/imu_vn100_spi.hxx"
#include "sensors/imu_vn100_uart.hxx"
#include "sensors/ugfile.hxx"

#include "imu_mgr.hxx"


//
// Global variables
//

static double imu_last_time = -31557600.0; // default to t minus one year old

static SGPropertyNode *imu_timestamp_node = NULL;

// comm property nodes
static SGPropertyNode *imu_console_skip = NULL;
static SGPropertyNode *imu_logging_skip = NULL;

static myprofile debug2a1;
static myprofile debug2a2;
	

void IMU_init() {
    debug2a1.set_name("debug2a1 IMU read");
    debug2a2.set_name("debug2a2 IMU console link");

    imu_timestamp_node = pyGetNode("/sensors/imu/time-stamp", true);

    // initialize comm nodes
    imu_console_skip = pyGetNode("/config/remote-link/imu-skip", true);
    imu_logging_skip = pyGetNode("/config/logging/imu-skip", true);

    // traverse configured modules
    SGPropertyNode *toplevel = pyGetNode("/config/sensors/imu-group", true);
    for ( int i = 0; i < toplevel->nChildren(); ++i ) {
	SGPropertyNode *section = toplevel->getChild(i);
	string name = section->getName();
	if ( name == "imu" ) {
	    string source = section->getChild("source", 0, true)->getString();
            bool enabled = section->getChild("enable", 0, true)->getBool();
            if ( !enabled ) {
                continue;
            }

	    string basename = "/sensors/";
	    basename += section->getDisplayName();
	    printf("i = %d  name = %s source = %s %s\n",
	           i, name.c_str(), source.c_str(), basename.c_str());
	    if ( source == "null" ) {
		// do nothing
	    } else if ( source == "APM2" ) {
		APM2_imu_init( basename, section );
	    } else if ( source == "Goldy2" ) {
		goldy2_imu_init( basename, section );
	    } else if ( source == "fgfs" ) {
		fgfs_imu_init( basename, section );
	    } else if ( source == "file" ) {
		ugfile_imu_init( basename, section );
	    } else if ( source == "vn100" ) {
		imu_vn100_uart_init( basename, section );
	    } else if ( source == "vn100-spi" ) {
		imu_vn100_spi_init( basename, section );
	    } else {
		printf("Unknown imu source = '%s' in config file\n",
		       source.c_str());
	    }
	}
    }
}


bool IMU_update() {
    debug2a1.start();

    imu_prof.start();

    bool fresh_data = false;

    // traverse configured modules
    SGPropertyNode *toplevel = pyGetNode("/config/sensors/imu-group", true);
    for ( int i = 0; i < toplevel->nChildren(); ++i ) {
	SGPropertyNode *section = toplevel->getChild(i);
	string name = section->getName();
	if ( name == "imu" ) {
	    string source = section->getChild("source", 0, true)->getString();
            bool enabled = section->getChild("enable", 0, true)->getBool();
            if ( !enabled ) {
                continue;
            }

	    // printf("i = %d  name = %s source = %s\n",
	    //        i, name.c_str(), source.c_str());
	    if ( source == "null" ) {
		// do nothing
	    } else if ( source == "APM2" ) {
		fresh_data = APM2_imu_update();
	    } else if ( source == "Goldy2" ) {
		fresh_data = goldy2_imu_update();
	    } else if ( source == "fgfs" ) {
		fresh_data = fgfs_imu_update();
	    } else if ( source == "file" ) {
		ugfile_read();
		fresh_data = ugfile_get_imu();
	    } else if ( source == "vn100" ) {
		fresh_data = imu_vn100_uart_get();
	    } else if ( source == "vn100-spi" ) {
		fresh_data = imu_vn100_spi_get();
	    } else {
		printf("Unknown imu source = '%s' in config file\n",
		       source.c_str());
	    }
	}
    }

    imu_prof.stop();
    debug2a1.stop();

    debug2a2.start();

    if ( fresh_data ) {
	// for computing imu data age
	imu_last_time = imu_timestamp_node->getDouble();

	if ( remote_link_on || log_to_file ) {
	    uint8_t buf[256];
	    int size = packetizer->packetize_imu( buf );

	    if ( remote_link_on ) {
		remote_link_imu( buf, size, imu_console_skip->getLong() );
	    }

	    if ( log_to_file ) {
		log_imu( buf, size, imu_logging_skip->getLong() );
	    }
	}
    }

    debug2a2.stop();

    return fresh_data;
}


void IMU_close() {
    // traverse configured modules
    SGPropertyNode *toplevel = pyGetNode("/config/sensors/imu-group", true);
    for ( int i = 0; i < toplevel->nChildren(); ++i ) {
	SGPropertyNode *section = toplevel->getChild(i);
	string name = section->getName();
	if ( name == "imu" ) {
	    string source = section->getChild("source", 0, true)->getString();
	    printf("i = %d  name = %s source = %s\n",
		   i, name.c_str(), source.c_str());
	    if ( source == "null" ) {
		// do nothing
	    } else if ( source == "APM2" ) {
		APM2_imu_close();
	    } else if ( source == "Goldy2" ) {
		goldy2_imu_close();
	    } else if ( source == "fgfs" ) {
		fgfs_imu_close();
	    } else if ( source == "file" ) {
		ugfile_close();
	    } else if ( source == "vn100" ) {
		imu_vn100_uart_close();
	    } else if ( source == "vn100-spi" ) {
		imu_vn100_spi_close();
	    } else {
		printf("Unknown imu source = '%s' in config file\n",
		       source.c_str());
	    }
	}
    }
}


double IMU_age() {
    return get_Time() - imu_last_time;
}
