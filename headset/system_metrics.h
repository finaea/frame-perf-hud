// The system sensor sources (ticket 02): CPU, GPU & RAM, Power, Battery, Temp, and General's uptime,
// clock and own cost. Each scheduler source calls one Sensors reader and stores its metrics, with an
// N/A reason for anything that can't be read. SensorBridge registers these next to its own sources.
#pragma once

#include "metric_store.h"
#include "scheduler.h"
#include "sensors.h"

/**
 * Registers a reader for each system source: CpuLoad, CpuClock, GpuClock, GpuBusy, Ram, Temps, BoardTemps,
 * Throttle, Fan, Power, Battery, Uptime, SelfCost.
 * @param scheduler where to register
 * @param sensors discover() already called; must outlive the scheduler
 * @param store where values go; must outlive the scheduler
 */
void registerSystemSources(fph::Scheduler& scheduler, Sensors& sensors, fph::MetricStore& store);
