// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - scripted smoke test driving the real UI with synthetic input.
#pragma once

class QString;

namespace gs {

class MainWindow;
// Runs the scripted session; returns the process exit code (0 = success).
int runSelfTest(MainWindow& w);
// Renders one frame offscreen with the real renderer (no window); prints diagnostics.
int runGlProbe();
// Times off-screen frames of a saved model with each render setting.
int runRenderBench(const QString& path, int frames);
// Times mouse moves with each tool over the open model (in a window).
int runInteractBench(MainWindow& w);
// Renders the README pictures into dir (see Screenshots.cpp).
int runScreenshots(MainWindow& w, const QString& dir);

} // namespace gs
