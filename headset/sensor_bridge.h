// Bridge between upstream's readers (Sensors, VrOverlay) and the metric registry.
// Each scheduler source calls one reader and stores its metrics in the MetricStore. The upstream panel
// then gets its SensorSample / FrameStats / ControllerStatus rebuilt from the store, so everything it
// shows has gone through the registry. Ticket 04 replaces the upstream panel with widgets that read the
// store directly.
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
     * @param sensors upstream sensors (discover() already called)
     * @param vr the overlay for frame timings and controllers, or nullptr when running without OpenVR
     * @param store where values go
     */
    SensorBridge(Sensors& sensors, VrOverlay* vr, fph::MetricStore& store);

    /** Registers one reader per source with the scheduler. */
    void registerSources(fph::Scheduler& scheduler);

    /** @return upstream's sensor sample, rebuilt from the store (N/A → NaN / false / empty) */
    SensorSample sample() const;
    /** @return upstream's frame stats, rebuilt from the store */
    FrameStats frame() const;
    /** @return upstream's controller status, rebuilt from the store */
    ControllerStatus controllers() const;

    /** The widgets the upstream panel shows. Ticket 04 replaces this with the saved layout. */
    static std::vector<fph::WidgetType> upstreamPanelWidgets();

private:
    Sensors& sensors_;
    VrOverlay* vr_;
    fph::MetricStore& store_;

    void readFrameTiming(double now);
    void readControllers(double now);
};
