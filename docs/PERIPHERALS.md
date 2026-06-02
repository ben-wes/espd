# YAML Peripheral & Actuator Mapping Guide

ESPD supports extending your board configuration with custom peripherals (like I2C/SPI sensors and actuators/steppers) by mapping them to Pure Data (Pd) symbols in your `boards/*.yaml` files.

Rather than embedding driver source code inside `gen_board_plugins.py`, ESPD employs a completely decoupled **BSP-Based Integration Design**:
1. All peripheral drivers are owned and compiled by your board's **BSP package** (esp-bsp).
2. The board YAML file simply **declares the mappings** between BSP sensor/actuator C functions and Pd receiver/sender symbols.
3. CMake automatically generates a background thread running on **Core 0** that handles polling sensors and invoking actuators, completely isolated from Core 1's audio loop to avoid audio stuttering.

---

## YAML Configuration Schema

Peripherals are declared in your `boards/*.yaml` file under the top-level `peripherals:` key:

```yaml
peripherals:
  - type: sensor
    name: acceleration_sensor
    bsp_getter: bsp_sensor_read_accel
    rate_ms: 10
    outputs:
      - espd/sensor/imu/accel/x
      - espd/sensor/imu/accel/y
      - espd/sensor/imu/accel/z

  - type: actuator
    name: stepper_motor
    bsp_setter: bsp_stepper_set_speed
    inputs:
      - espd/motor/0/speed
```

---

## Mapping Details

### 1. `sensor`
Periodically calls a BSP getter function to read physical values and publish them to Pd.

#### Required Keys
- `bsp_getter`: The name of the BSP function to read the values. The function should accept output pointers as arguments (e.g. `esp_err_t bsp_sensor_read_accel(float *x, float *y, float *z)`).
- `rate_ms`: Polling interval in milliseconds.
- `outputs`: A list of Pd symbol names. The generated task will pass pointers to variables in order to the `bsp_getter` function and post the results to these symbols.

---

### 2. `actuator`
Binds a Pd receiver to incoming float values, forwarding them directly to a BSP driver function.

#### Required Keys
- `bsp_setter`: The name of the BSP function to write values (e.g. `void bsp_stepper_set_speed(float speed)`).
- `inputs`: A list of Pd symbols that trigger the `bsp_setter` function when updated in the Pd patch.

---

## Benefits of BSP-Based Integration
- **Driver Independence**: Add support for any I2C/SPI/I2S device (or stepper motor) by implementing its driver inside the BSP component. No changes to the `espd` firmware compiler or build scripts are needed!
- **Pure Core Isolation**: All generated getter/setter polling task code runs on **Core 0**, keeping Core 1's high-prio audio thread glitch-free.
