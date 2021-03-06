/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
 * Copyright (C) 2016  Intel Corporation. All rights reserved.
 *
 * This file is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "AP_Compass_BMM150.h"

#include <AP_HAL/AP_HAL.h>

#if CONFIG_HAL_BOARD == HAL_BOARD_LINUX

#include <utility>

#include <AP_HAL/utility/sparse-endian.h>
#include <AP_Math/AP_Math.h>

#define CHIP_ID_REG 0x40
#define CHIP_ID_VAL 0x32

#define POWER_AND_OPERATIONS_REG 0x4B
#define POWER_CONTROL_VAL (1 << 0)
#define SOFT_RESET (1 << 7 | 1 << 1)

#define OP_MODE_SELF_TEST_ODR_REG 0x4C
#define NORMAL_MODE (0 << 1)
#define ODR_30HZ (1 << 3 | 1 << 4 | 1 << 5)
#define ODR_20HZ (1 << 3 | 0 << 4 | 1 << 5)

#define DATA_X_LSB_REG 0x42

#define REPETITIONS_XY_REG 0x51
#define REPETITIONS_Z_REG 0X52

/* Trim registers */
#define DIG_X1_REG 0x5D
#define DIG_Y1_REG 0x5E
#define DIG_Z4_LSB_REG 0x62
#define DIG_Z4_MSB_REG 0x63
#define DIG_X2_REG 0x64
#define DIG_Y2_REG 0x65
#define DIG_Z2_LSB_REG 0x68
#define DIG_Z2_MSB_REG 0x69
#define DIG_Z1_LSB_REG 0x6A
#define DIG_Z1_MSB_REG 0x6B
#define DIG_XYZ1_LSB_REG 0x6C
#define DIG_XYZ1_MSB_REG 0x6D
#define DIG_Z3_LSB_REG 0x6E
#define DIG_Z3_MSB_REG 0x6F
#define DIG_XY2_REG 0x70
#define DIG_XY1_REG 0x71

#define MEASURE_TIME_USEC 10000

extern const AP_HAL::HAL &hal;

AP_Compass_Backend *AP_Compass_BMM150::probe(Compass &compass,
                                             AP_HAL::OwnPtr<AP_HAL::I2CDevice> dev)
{
    AP_Compass_BMM150 *sensor = new AP_Compass_BMM150(compass, std::move(dev));
    if (!sensor || !sensor->init()) {
        delete sensor;
        return nullptr;
    }

    return sensor;
}

AP_Compass_BMM150::AP_Compass_BMM150(Compass &compass,
                                     AP_HAL::OwnPtr<AP_HAL::Device> dev)
    : AP_Compass_Backend(compass)
    , _dev(std::move(dev))
{
}

bool AP_Compass_BMM150::_load_trim_values()
{
    struct {
        int8_t dig_x1;
        int8_t dig_y1;
        uint8_t rsv[3];
        le16_t dig_z4;
        int8_t dig_x2;
        int8_t dig_y2;
        uint8_t rsv2[2];
        le16_t dig_z2;
        le16_t dig_z1;
        le16_t dig_xyz1;
        le16_t dig_z3;
        int8_t dig_xy2;
        uint8_t dig_xy1;
    } PACKED trim_registers;

    if (!_dev->read_registers(DIG_X1_REG, (uint8_t *)&trim_registers,
                              sizeof(trim_registers))) {
        return false;
    }

    _dig.x1 = trim_registers.dig_x1;
    _dig.x2 = trim_registers.dig_x2;
    _dig.xy1 = trim_registers.dig_xy1;
    _dig.xy2 = trim_registers.dig_xy2;
    _dig.xyz1 = le16toh(trim_registers.dig_xyz1);
    _dig.y1 = trim_registers.dig_y1;
    _dig.y2 = trim_registers.dig_y2;
    _dig.z1 = le16toh(trim_registers.dig_z1);
    _dig.z2 = le16toh(trim_registers.dig_z2);
    _dig.z3 = le16toh(trim_registers.dig_z3);
    _dig.z4 = le16toh(trim_registers.dig_z4);

    return true;
}

bool AP_Compass_BMM150::init()
{
    uint8_t val = 0;
    bool ret;

    hal.scheduler->suspend_timer_procs();

    if (!_dev->get_semaphore()->take(HAL_SEMAPHORE_BLOCK_FOREVER)) {
        hal.console->printf("BMM150: Unable to get bus semaphore\n");
        goto fail_sem;
    }

    /* Do a soft reset */
    ret = _dev->write_register(POWER_AND_OPERATIONS_REG, SOFT_RESET);
    if (!ret) {
        goto bus_error;
    }
    hal.scheduler->delay(2);

    /* Change power state from suspend mode to sleep mode */
    ret = _dev->write_register(POWER_AND_OPERATIONS_REG, POWER_CONTROL_VAL);
    if (!ret) {
        goto bus_error;
    }
    hal.scheduler->delay(2);

    ret = _dev->read_registers(CHIP_ID_REG, &val, 1);
    if (!ret) {
        goto bus_error;
    }
    if (val != CHIP_ID_VAL) {
        hal.console->printf("BMM150: Wrong id\n");
        goto fail;
    }

    ret = _load_trim_values();
    if (!ret) {
        goto bus_error;
    }

    /*
     * Recommended preset for high accuracy:
     * - Rep X/Y = 47
     * - Rep Z = 83
     * - ODR = 20
     * But we are going to use 30Hz of ODR
     */
    ret = _dev->write_register(REPETITIONS_XY_REG, (47 - 1) / 2);
    if (!ret) {
        goto bus_error;
    }
    ret = _dev->write_register(REPETITIONS_Z_REG, 83 - 1);
    if (!ret) {
        goto bus_error;
    }
    /* Change operation mode from sleep to normal and set ODR */
    ret = _dev->write_register(OP_MODE_SELF_TEST_ODR_REG, NORMAL_MODE | ODR_30HZ);
    if (!ret) {
        goto bus_error;
    }

    _dev->get_semaphore()->give();
    hal.scheduler->resume_timer_procs();

    /* register the compass instance in the frontend */
    _compass_instance = register_compass();
    set_dev_id(_compass_instance, AP_COMPASS_TYPE_BMM150);

    hal.scheduler->register_timer_process(FUNCTOR_BIND_MEMBER(&AP_Compass_BMM150::_update, void));

    return true;

bus_error:
    hal.console->printf("BMM150: Bus communication error\n");
fail:
    _dev->get_semaphore()->give();
fail_sem:
    hal.scheduler->resume_timer_procs();
    return false;
}

/*
 * Compensation algorithm got from https://github.com/BoschSensortec/BMM050_driver
 * this is not explained in datasheet.
 */
int16_t AP_Compass_BMM150::_compensate_xy(int16_t xy, uint32_t rhall, int32_t txy1, int32_t txy2)
{
    int32_t inter = ((int32_t)_dig.xyz1) << 14;
    inter /= rhall;
    inter -= 0x4000;

    int32_t val = _dig.xy2 * ((inter * inter) >> 7);
    val += (inter * (((uint32_t)_dig.xy1) << 7));
    val >>= 9;
    val += 0x100000;
    val *= (txy2 + 0xA0);
    val >>= 12;
    val *= xy;
    val >>= 13;
    val += (txy1 << 3);

    return val;
}

int16_t AP_Compass_BMM150::_compensate_z(int16_t z, uint32_t rhall)
{
    int32_t dividend = ((int32_t)(z - _dig.z4)) << 15;
    dividend -= (_dig.z3 * (rhall - _dig.xyz1)) >> 2;

    int32_t divisor = ((int32_t)_dig.z1) * (rhall << 1);
    divisor += 0x8000;
    divisor >>= 16;
    divisor += _dig.z2;

    return constrain_int32(dividend / divisor, -0x8000, 0x8000);
}

void AP_Compass_BMM150::_update()
{
    const uint32_t time_usec = AP_HAL::micros();

    if (time_usec - _last_update_timestamp < MEASURE_TIME_USEC) {
        return;
    }

    if (!_dev->get_semaphore()->take_nonblocking()) {
        return;
    }

    le16_t data[4];
    bool ret = _dev->read_registers(DATA_X_LSB_REG, (uint8_t *) &data, sizeof(data));
    _dev->get_semaphore()->give();

    /* Checking data ready status */
    if (!ret || !(data[3] & 0x1)) {
        return;
    }

    const uint16_t rhall = le16toh(data[3] >> 2);

    Vector3f raw_field = Vector3f{
        (float) _compensate_xy(((int16_t)le16toh(data[0])) >> 3,
                               rhall, _dig.x1, _dig.x2),
        (float) _compensate_xy(((int16_t)le16toh(data[1])) >> 3,
                               rhall, _dig.y1, _dig.y2),
        (float) _compensate_z(((int16_t)le16toh(data[2])) >> 1, rhall)};

    /* apply sensitivity scale 16 LSB/uT */
    raw_field /= 16;
    /* convert uT to milligauss */
    raw_field *= 10;

    /* rotate raw_field from sensor frame to body frame */
    rotate_field(raw_field, _compass_instance);

    /* publish raw_field (uncorrected point sample) for calibration use */
    publish_raw_field(raw_field, time_usec, _compass_instance);

    /* correct raw_field for known errors */
    correct_field(raw_field, _compass_instance);

    _mag_accum += raw_field;
    _accum_count++;
    if (_accum_count == 10) {
        _mag_accum /= 2;
        _accum_count = 5;
    }

    _last_update_timestamp = time_usec;
}

void AP_Compass_BMM150::read()
{
    if (_accum_count == 0) {
        return;
    }

    hal.scheduler->suspend_timer_procs();
    Vector3f field(_mag_accum);
    field /= _accum_count;
    _mag_accum.zero();
    _accum_count = 0;
    hal.scheduler->resume_timer_procs();

    publish_filtered_field(field, _compass_instance);
}

#endif
