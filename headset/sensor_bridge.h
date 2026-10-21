// Bridge between the readers (Sensors, VrOverlay) and the metric registry. Each scheduler source calls one reader
// and stores its metrics in the MetricStore; the widgets read the store.
#pragma once

#include "history.h"
#include "metric_store.h"
#include "scheduler.h"
#include "sensors.h"

#include <vector>

class VrOverlay;

class SensorBridge {
public:
    /**
     * @param sensors the sensors (discover() already called)
     * @param vr the overlay for frame timings and controllers, or nullptr when running without OpenVR
     * @param store where values go
     */
    SensorBridge(Sensors& sensors, VrOverlay* vr, frame_perf_overlay::MetricStore& store);

    /** Registers one reader per source with the scheduler. */
    void registerSources(frame_perf_overlay::Scheduler& scheduler);

    /** @return the sensor sample, rebuilt from the store (N/A → NaN / false / empty), for --print */
    SensorSample sample() const;

    /** @return every widget type, for --print (reads every system source) */
    static std::vector<frame_perf_overlay::WidgetType> allWidgets();

private:
    Sensors& sensors_;
    VrOverlay* vr_;
    frame_perf_overlay::MetricStore& store_;

    void readFrameTiming(double now);
    void readControllers(double now);
};
