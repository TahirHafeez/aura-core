/**
 * \file: airdata_mgr.hxx
 *
 * Front end management interface for reading air data.
 *
 * Copyright (C) 2009 - Curtis L. Olson curtolson@flightgear.org
 *
 */

#pragma once

void AirData_init();
bool AirData_update();
void AirData_calibrate();
void AirData_close();
