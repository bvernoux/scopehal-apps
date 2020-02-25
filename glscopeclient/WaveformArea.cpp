/***********************************************************************************************************************
*                                                                                                                      *
* ANTIKERNEL v0.1                                                                                                      *
*                                                                                                                      *
* Copyright (c) 2012-2020 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief  Implementation of WaveformArea
 */
#include "glscopeclient.h"
#include "WaveformArea.h"
#include "OscilloscopeWindow.h"
#include <random>
#include "ProfileBlock.h"
#include "../../lib/scopeprotocols/scopeprotocols.h"

using namespace std;
using namespace glm;

bool WaveformArea::m_firstGLContextRun = false;

WaveformArea::WaveformArea(
	Oscilloscope* scope,
	OscilloscopeChannel* channel,
	OscilloscopeWindow* parent
	)
	: m_persistence(false)
	, m_scope(scope)
	, m_channel(channel)
	, m_parent(parent)
	, m_pixelsPerVolt(1)
{
	SharedCtorInit();
}

/**
	@brief Semi-copy constructor, used when copying a waveform to a new group

	Note that we only clone UI settings, the GL context, GTK properties, etc are new!
 */
WaveformArea::WaveformArea(const WaveformArea* clone)
	: m_persistence(clone->m_persistence)
	, m_scope(clone->m_scope)
	, m_channel(clone->m_channel)
	, m_parent(clone->m_parent)
	, m_pixelsPerVolt(clone->m_pixelsPerVolt)
{
	SharedCtorInit();
}

void WaveformArea::SharedCtorInit()
{
	//performance counters
	m_frameTime 			= 0;
	m_frameCount 			= 0;
	m_renderTime 			= 0;
	m_prepareTime 			= 0;
	m_downloadTime 			= 0;
	m_cairoTime				= 0;
	m_texDownloadTime		= 0;
	m_compositeTime			= 0;
	m_indexTime 			= 0;
	m_lastFrameStart 		= -1;

	m_updatingContextMenu 	= false;
	m_selectedChannel		= m_channel;
	m_dragState 			= DRAG_NONE;
	m_padding 				= 2;
	m_persistenceClear 		= true;
	m_firstFrame 			= false;
	m_waveformRenderData	= NULL;

	set_has_alpha();

	add_events(
		Gdk::EXPOSURE_MASK |
		Gdk::POINTER_MOTION_MASK |
		Gdk::SCROLL_MASK |
		Gdk::BUTTON_PRESS_MASK |
		Gdk::BUTTON_RELEASE_MASK);

	CreateWidgets();

	m_group = NULL;

	m_channel->AddRef();
}

WaveformArea::~WaveformArea()
{
	LogDebug("Shutting down view for waveform %s\n", m_channel->m_displayname.c_str());
	{
		LogIndenter li;

		double tavg = m_frameTime / m_frameCount;
		LogDebug("Average frame interval: %.3f ms (%.2f FPS, %zu frames)\n",
			tavg*1000, 1/tavg, m_frameCount);

		LogDebug("----------------------------------------------------------\n");
		LogDebug("Task              | Total (ms) | Average (ms) | Percentage\n");
		LogDebug("----------------------------------------------------------\n");
		LogDebug("Render            | %10.1f |   %10.3f | %.1f %%\n",
			m_renderTime * 1000, m_renderTime * 1000 / m_frameCount, 100.0f);
		LogDebug("Cairo             | %10.1f |   %10.3f | %.1f %%\n",
			m_cairoTime * 1000, m_cairoTime * 1000 / m_frameCount, m_cairoTime * 100 / m_renderTime);
		LogDebug("Texture download  | %10.1f |   %10.3f | %.1f %%\n",
			m_texDownloadTime * 1000, m_texDownloadTime * 1000 / m_frameCount, m_texDownloadTime * 100 / m_renderTime);
		LogDebug("Prepare           | %10.1f |   %10.3f | %.1f %%\n",
			m_prepareTime * 1000, m_prepareTime * 1000 / m_frameCount, m_prepareTime * 100 / m_renderTime);
		LogDebug("Build index       | %10.1f |   %10.3f | %.1f %%\n",
			m_indexTime * 1000, m_indexTime * 1000 / m_frameCount, m_indexTime * 100 / m_renderTime);
		LogDebug("Geometry download | %10.1f |   %10.3f | %.1f %%\n",
			m_downloadTime * 1000, m_downloadTime * 1000 / m_frameCount, m_downloadTime * 100 / m_renderTime);
		LogDebug("Composite         | %10.1f |   %10.3f | %.1f %%\n",
			m_compositeTime * 1000, m_compositeTime * 1000 / m_frameCount, m_compositeTime * 100 / m_renderTime);
	}

	m_channel->Release();

	for(auto d : m_overlays)
		OnRemoveOverlay(d);
	m_overlays.clear();

	for(auto m : m_moveExistingGroupItems)
	{
		m_moveMenu.remove(*m);
		delete m;
	}
	m_moveExistingGroupItems.clear();
}

void WaveformArea::OnRemoveOverlay(ProtocolDecoder* decode)
{
	//If we're about to remove the last reference to a decoder, make sure the parent knows
	if(decode->GetRefCount() == 1)
		m_parent->RemoveDecoder(decode);

	//Remove the render data for it
	auto it = m_overlayRenderData.find(decode);
	if(it != m_overlayRenderData.end())
		m_overlayRenderData.erase(it);

	decode->Release();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Initialization

void WaveformArea::CreateWidgets()
{
	//Delete
	auto item = Gtk::manage(new Gtk::MenuItem("Delete", false));
		item->signal_activate().connect(
			sigc::mem_fun(*this, &WaveformArea::OnHide));
		m_contextMenu.append(*item);

	//Move/copy
	m_contextMenu.append(m_moveItem);
		m_moveItem.set_label("Move waveform to");
		m_moveItem.set_submenu(m_moveMenu);
			m_moveMenu.append(m_moveNewGroupBelowItem);
				m_moveNewGroupBelowItem.set_label("Insert new group at bottom");
				m_moveNewGroupBelowItem.signal_activate().connect(
					sigc::mem_fun(*this, &WaveformArea::OnMoveNewBelow));
			m_moveMenu.append(m_moveNewGroupRightItem);
				m_moveNewGroupRightItem.set_label("Insert new group at right");
				m_moveNewGroupRightItem.signal_activate().connect(
					sigc::mem_fun(*this, &WaveformArea::OnMoveNewRight));
			m_moveMenu.append(*Gtk::manage(new Gtk::SeparatorMenuItem));
	m_contextMenu.append(m_copyItem);
		m_copyItem.set_label("Copy waveform to");
		m_copyItem.set_submenu(m_copyMenu);
			m_copyMenu.append(m_copyNewGroupBelowItem);
				m_copyNewGroupBelowItem.set_label("Insert new group at bottom");
				m_copyNewGroupBelowItem.signal_activate().connect(
					sigc::mem_fun(*this, &WaveformArea::OnCopyNewBelow));
			m_copyMenu.append(m_copyNewGroupRightItem);
				m_copyNewGroupRightItem.set_label("Insert new group at right");
				m_copyNewGroupRightItem.signal_activate().connect(
					sigc::mem_fun(*this, &WaveformArea::OnCopyNewRight));

	//Persistence
	m_contextMenu.append(m_persistenceItem);
		m_persistenceItem.set_label("Persistence");
		m_persistenceItem.signal_activate().connect(
			sigc::mem_fun(*this, &WaveformArea::OnTogglePersistence));

	m_contextMenu.append(*Gtk::manage(new Gtk::SeparatorMenuItem));

	//Cursor
	m_contextMenu.append(m_cursorItem);
		m_cursorItem.set_label("Cursor");
		m_cursorItem.set_submenu(m_cursorMenu);
			m_cursorMenu.append(m_cursorNoneItem);
				m_cursorNoneItem.set_label("None");
				m_cursorNoneItem.set_group(m_cursorGroup);
				m_cursorNoneItem.signal_activate().connect(
					sigc::bind<WaveformGroup::CursorConfig, Gtk::RadioMenuItem*>(
						sigc::mem_fun(*this, &WaveformArea::OnCursorConfig),
						WaveformGroup::CURSOR_NONE,
						&m_cursorNoneItem));
			m_cursorMenu.append(m_cursorSingleVerticalItem);
				m_cursorSingleVerticalItem.set_label("Vertical (single)");
				m_cursorSingleVerticalItem.set_group(m_cursorGroup);
				m_cursorSingleVerticalItem.signal_activate().connect(
					sigc::bind<WaveformGroup::CursorConfig, Gtk::RadioMenuItem*>(
						sigc::mem_fun(*this, &WaveformArea::OnCursorConfig),
						WaveformGroup::CURSOR_X_SINGLE,
						&m_cursorSingleVerticalItem));
			m_cursorMenu.append(m_cursorDualVerticalItem);
				m_cursorDualVerticalItem.set_label("Vertical (dual)");
				m_cursorDualVerticalItem.set_group(m_cursorGroup);
				m_cursorDualVerticalItem.signal_activate().connect(
					sigc::bind<WaveformGroup::CursorConfig, Gtk::RadioMenuItem*>(
						sigc::mem_fun(*this, &WaveformArea::OnCursorConfig),
						WaveformGroup::CURSOR_X_DUAL,
						&m_cursorDualVerticalItem));

	m_contextMenu.append(*Gtk::manage(new Gtk::SeparatorMenuItem));

	//Trigger
	m_contextMenu.append(m_triggerItem);
		m_triggerItem.set_label("Trigger");
		m_triggerItem.set_submenu(m_triggerMenu);
			m_risingTriggerItem.set_label("Rising edge");
			m_risingTriggerItem.signal_activate().connect(
				sigc::bind<Oscilloscope::TriggerType, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnTriggerMode),
					Oscilloscope::TRIGGER_TYPE_RISING,
					&m_risingTriggerItem));
			m_risingTriggerItem.set_group(m_triggerGroup);
			m_triggerMenu.append(m_risingTriggerItem);

			m_fallingTriggerItem.set_label("Falling edge");
			m_fallingTriggerItem.signal_activate().connect(
				sigc::bind<Oscilloscope::TriggerType, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnTriggerMode),
					Oscilloscope::TRIGGER_TYPE_FALLING,
					&m_fallingTriggerItem));
			m_fallingTriggerItem.set_group(m_triggerGroup);
			m_triggerMenu.append(m_fallingTriggerItem);

			m_bothTriggerItem.set_label("Both edges");
			m_bothTriggerItem.signal_activate().connect(
				sigc::bind<Oscilloscope::TriggerType, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnTriggerMode),
					Oscilloscope::TRIGGER_TYPE_CHANGE,
					&m_bothTriggerItem));
			m_bothTriggerItem.set_group(m_triggerGroup);
			m_triggerMenu.append(m_bothTriggerItem);

	m_contextMenu.append(*Gtk::manage(new Gtk::SeparatorMenuItem));

	//Attenuation
	m_contextMenu.append(m_attenItem);
		m_attenItem.set_label("Attenuation");
		m_attenItem.set_submenu(m_attenMenu);
			m_atten1xItem.set_label("1x");
				m_atten1xItem.set_group(m_attenGroup);
				m_attenMenu.append(m_atten1xItem);
			m_atten10xItem.set_label("10x");
				m_atten10xItem.set_group(m_attenGroup);
				m_attenMenu.append(m_atten10xItem);
			m_atten20xItem.set_label("20x");
				m_atten20xItem.set_group(m_attenGroup);
				m_attenMenu.append(m_atten20xItem);

	//Bandwidth
	m_contextMenu.append(m_bwItem);
		m_bwItem.set_label("Bandwidth");
		m_bwItem.set_submenu(m_bwMenu);
			m_bwFullItem.set_label("Full");
				m_bwFullItem.set_group(m_bwGroup);
				m_bwFullItem.signal_activate().connect(sigc::bind<int, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnBandwidthLimit), 0, &m_bwFullItem));
				m_bwMenu.append(m_bwFullItem);
			m_bw200Item.set_label("200 MHz");
				m_bw200Item.set_group(m_bwGroup);
				m_bw200Item.signal_activate().connect(sigc::bind<int, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnBandwidthLimit), 200, &m_bw200Item));
				m_bwMenu.append(m_bw200Item);
			m_bw20Item.set_label("20 MHz");
				m_bw20Item.set_group(m_bwGroup);
				m_bw20Item.signal_activate().connect(sigc::bind<int, Gtk::RadioMenuItem*>(
					sigc::mem_fun(*this, &WaveformArea::OnBandwidthLimit), 20, &m_bw20Item));
				m_bwMenu.append(m_bw20Item);

	//Coupling
	m_contextMenu.append(m_couplingItem);
		m_couplingItem.set_label("Coupling");
		m_couplingItem.set_submenu(m_couplingMenu);
			m_ac1MCouplingItem.set_label("AC 1M");
				m_ac1MCouplingItem.set_group(m_couplingGroup);
				m_couplingMenu.append(m_ac1MCouplingItem);
			m_dc1MCouplingItem.set_label("DC 1M");
				m_dc1MCouplingItem.set_group(m_couplingGroup);
				m_couplingMenu.append(m_dc1MCouplingItem);
			m_dc50CouplingItem.set_label("DC 50Ω");
				m_dc50CouplingItem.set_group(m_couplingGroup);
				m_couplingMenu.append(m_dc50CouplingItem);
			m_gndCouplingItem.set_label("GND");
				m_gndCouplingItem.set_group(m_couplingGroup);
				m_couplingMenu.append(m_gndCouplingItem);

	m_contextMenu.append(*Gtk::manage(new Gtk::SeparatorMenuItem));

	//Decode
	m_contextMenu.append(m_decodeItem);
		m_decodeItem.set_label("Filter");
		m_decodeItem.set_submenu(m_decodeMenu);
		m_decodeMenu.append(m_decodeAnalysisItem);
			m_decodeAnalysisItem.set_label("Analysis");
			m_decodeAnalysisItem.set_submenu(m_decodeAnalysisMenu);
		m_decodeMenu.append(m_decodeClockItem);
			m_decodeClockItem.set_label("Clocking");
			m_decodeClockItem.set_submenu(m_decodeClockMenu);
		m_decodeMenu.append(m_decodeConversionItem);
			m_decodeConversionItem.set_label("Conversion");
			m_decodeConversionItem.set_submenu(m_decodeConversionMenu);
		m_decodeMenu.append(m_decodeMathItem);
			m_decodeMathItem.set_label("Math");
			m_decodeMathItem.set_submenu(m_decodeMathMenu);
		m_decodeMenu.append(m_decodeMeasurementItem);
			m_decodeMeasurementItem.set_label("Measurement");
			m_decodeMeasurementItem.set_submenu(m_decodeMeasurementMenu);
		m_decodeMenu.append(m_decodeMiscItem);
			m_decodeMiscItem.set_label("Misc");
			m_decodeMiscItem.set_submenu(m_decodeMiscMenu);
		m_decodeMenu.append(m_decodeSerialItem);
			m_decodeSerialItem.set_label("Serial");
			m_decodeSerialItem.set_submenu(m_decodeSerialMenu);


		vector<string> names;
		ProtocolDecoder::EnumProtocols(names);
		for(auto p : names)
		{
			item = Gtk::manage(new Gtk::MenuItem(p, false));
			item->signal_activate().connect(
				sigc::bind<string>(sigc::mem_fun(*this, &WaveformArea::OnProtocolDecode), p));

			//Create a test decode and see where it goes
			auto d = ProtocolDecoder::CreateDecoder(p, "");
			switch(d->GetCategory())
			{
				case ProtocolDecoder::CAT_ANALYSIS:
					m_decodeAnalysisMenu.append(*item);
					break;

				case ProtocolDecoder::CAT_CLOCK:
					m_decodeClockMenu.append(*item);
					break;

				case ProtocolDecoder::CAT_CONVERSION:
					m_decodeConversionMenu.append(*item);
					break;

				case ProtocolDecoder::CAT_MEASUREMENT:
					m_decodeMeasurementMenu.append(*item);
					break;

				case ProtocolDecoder::CAT_MATH:
					m_decodeMathMenu.append(*item);
					break;

				case ProtocolDecoder::CAT_SERIAL:
					m_decodeSerialMenu.append(*item);
					break;

				default:
				case ProtocolDecoder::CAT_MISC:
					m_decodeMiscMenu.append(*item);
					break;
			}
			delete d;
		}

	//Measurements
	m_contextMenu.append(m_measureItem);
		m_measureItem.set_label("Measure");
		m_measureItem.set_submenu(m_measureMenu);

		m_measureMenu.append(m_measureHorzItem);
			m_measureHorzItem.set_label("Horizontal");
			m_measureHorzItem.set_submenu(m_measureHorzMenu);
		m_measureMenu.append(m_measureVertItem);
			m_measureVertItem.set_label("Vertical");
			m_measureVertItem.set_submenu(m_measureVertMenu);

		names.clear();
		Measurement::EnumMeasurements(names);
		for(auto p : names)
		{
			item = Gtk::manage(new Gtk::MenuItem(p, false));
			item->signal_activate().connect(
				sigc::bind<string>(sigc::mem_fun(*this, &WaveformArea::OnMeasure), p));

			auto m = Measurement::CreateMeasurement(p);
			switch(m->GetMeasurementType())
			{
				case Measurement::MEAS_HORZ:
					m_measureHorzMenu.append(*item);
					break;

				case Measurement::MEAS_VERT:
					m_measureVertMenu.append(*item);
					break;
			}

			delete m;
		}

	m_contextMenu.show_all();
}

void WaveformArea::on_realize()
{
	//Let the base class create the GL context, then select it
	Gtk::GLArea::on_realize();
	make_current();

	/* FIXME: This code does not currently function correctly.
	 *
	 * github issue reference: https://github.com/azonenberg/scopehal-apps/issues/66
	 *
	 * Issues:
	 *
	 * - The version returned is not the highest OpenGL version the system supports.
	 *   This is due to how GTK is creating the OpenGL context, and how OpenGL deals with backwards compatibility.
	 *   We should probably have GTK create the OpenGL context, requesting version 4.3 as the minimum required version.
	 *
	 *   Reference Links:
	 *   - https://stackoverflow.com/questions/47151394/how-to-tell-which-version-of-opengl-my-graphics-card-supports-on-linux/47166829#47166829
	 *   - https://stackoverflow.com/questions/46510889/how-can-i-know-which-opengl-version-is-supported-by-my-system/46511741#46511741
	 *   - https://cgit.freedesktop.org/mesa/demos/tree/src/xdemos/glxinfo.c
	 *
	 */

	//Only run on the very first GL context
	if ( !m_firstGLContextRun )
	{
		m_firstGLContextRun = true;

		//Check the OpenGL minimum version requirement
		int versionMajor;
		int versionMinor;

 #if 0
		//Legacy method
		stringstream ss((const char *) glGetString(GL_VERSION));
		ss >> versionMajor;
		ss.ignore(1);
		ss >> versionMinor;
 #else

		//New Method
		glGetIntegerv(GL_MAJOR_VERSION, &versionMajor);
		glGetIntegerv(GL_MINOR_VERSION, &versionMinor);
 #endif

		printf("OpenGL Version: %d.%d (%s)\n", versionMajor, versionMinor, (const char *) glGetString(GL_VERSION) );

		if ( versionMajor < 4 || (versionMajor == 4 && versionMinor < 3) )
		{
			LogError("ERROR: OpenGL version too old, requires >= 4.3 (found %d.%d)\n", versionMajor, versionMinor);
			exit(1);
		}
	}

	//We're about to draw the first frame after realization.
	//This means we need to save some configuration (like the current FBO) that GTK doesn't tell us directly
	m_firstFrame = true;

	//Create waveform render data for our main trace
	m_waveformRenderData = new WaveformRenderData(m_channel);

	//Set stuff up for each rendering pass
	InitializeWaveformPass();
	InitializeColormapPass();
	InitializePersistencePass();
	InitializeCairoPass();
	InitializeEyePass();
}

void WaveformArea::on_unrealize()
{
	make_current();

	CleanupGLHandles();

	Gtk::GLArea::on_unrealize();
}

void WaveformArea::CleanupGLHandles()
{
	//Clean up old shaders
	m_waveformComputeProgram.Destroy();
	m_colormapProgram.Destroy();
	m_persistProgram.Destroy();
	m_eyeProgram.Destroy();
	m_cairoProgram.Destroy();

	//Clean up old VAOs
	m_colormapVAO.Destroy();
	m_persistVAO.Destroy();
	m_cairoVAO.Destroy();
	m_eyeVAO.Destroy();

	//Clean up old VBOs
	m_colormapVBO.Destroy();
	m_persistVBO.Destroy();
	m_cairoVBO.Destroy();
	m_eyeVBO.Destroy();

	//Clean up old textures
	m_cairoTexture.Destroy();
	m_cairoTextureOver.Destroy();
	for(auto& e : m_eyeColorRamp)
		e.Destroy();

	delete m_waveformRenderData;
	m_waveformRenderData = NULL;
	for(auto it : m_overlayRenderData)
		delete it.second;
	m_overlayRenderData.clear();

	//Detach the FBO so we don't destroy it!!
	//GTK manages this, and it might be used by more than one waveform area within the application.
	m_windowFramebuffer.Detach();
}

void WaveformArea::InitializeWaveformPass()
{
	//ProfileBlock pb("Load waveform shaders");
	ComputeShader wc;
	if(!wc.Load("shaders/waveform-compute.glsl"))
		LogFatal("failed to load waveform compute shader, aborting\n");
	m_waveformComputeProgram.Add(wc);
	if(!m_waveformComputeProgram.Link())
		LogFatal("failed to link shader program, aborting\n");
}

void WaveformArea::InitializeColormapPass()
{
	//ProfileBlock pb("Load colormap shaders");

	//Set up shaders
	VertexShader cvs;
	FragmentShader cfs;
	if(!cvs.Load("shaders/colormap-vertex.glsl") || !cfs.Load("shaders/colormap-fragment.glsl"))
		LogFatal("failed to load colormap shaders, aborting\n");

	m_colormapProgram.Add(cvs);
	m_colormapProgram.Add(cfs);
	if(!m_colormapProgram.Link())
		LogFatal("failed to link shader program, aborting\n");

	//Create the VAO/VBO for a fullscreen polygon
	float verts[8] =
	{
		-1, -1,
		 1, -1,
		 1,  1,
		-1,  1
	};
	m_colormapVBO.Bind();
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	m_colormapVAO.Bind();
	m_colormapProgram.EnableVertexArray("vert");
	m_colormapProgram.SetVertexAttribPointer("vert", 2, 0);
}

void WaveformArea::InitializeEyePass()
{
	//Set up shaders
	VertexShader cvs;
	FragmentShader cfs;
	if(!cvs.Load("shaders/eye-vertex.glsl") || !cfs.Load("shaders/eye-fragment.glsl"))
		LogFatal("failed to load eye shaders, aborting\n");

	m_eyeProgram.Add(cvs);
	m_eyeProgram.Add(cfs);
	if(!m_eyeProgram.Link())
		LogFatal("failed to link shader program, aborting\n");

	//Create the VAO/VBO for a fullscreen polygon
	float verts[8] =
	{
		-1, -1,
		 1, -1,
		 1,  1,
		-1,  1
	};
	m_eyeVBO.Bind();
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	m_eyeVAO.Bind();
	m_eyeProgram.EnableVertexArray("vert");
	m_eyeProgram.SetVertexAttribPointer("vert", 2, 0);

	//Load the eye color ramp
	char tmp[1024];
	const char* fnames[OscilloscopeWindow::NUM_EYE_COLORS];
	fnames[OscilloscopeWindow::EYE_CRT] = "gradients/eye-gradient-crt.rgba";
	fnames[OscilloscopeWindow::EYE_IRONBOW] = "gradients/eye-gradient-ironbow.rgba";
	fnames[OscilloscopeWindow::EYE_KRAIN] = "gradients/eye-gradient-krain.rgba";
	fnames[OscilloscopeWindow::EYE_RAINBOW] = "gradients/eye-gradient-rainbow.rgba";
	fnames[OscilloscopeWindow::EYE_GRAYSCALE] = "gradients/eye-gradient-grayscale.rgba";
	fnames[OscilloscopeWindow::EYE_VIRIDIS] = "gradients/eye-gradient-viridis.rgba";
	for(int i=0; i<OscilloscopeWindow::NUM_EYE_COLORS; i++)
	{
		FILE* fp = fopen(fnames[i], "r");
		if(!fp)
			LogFatal("fail to open eye gradient");
		fread(tmp, 1, 1024, fp);
		fclose(fp);

		m_eyeColorRamp[i].Bind();
		ResetTextureFiltering();
		m_eyeColorRamp[i].SetData(256, 1, tmp, GL_RGBA);
	}
}

void WaveformArea::InitializePersistencePass()
{
	//ProfileBlock pb("Load persistence shaders");

	//Set up shaders
	VertexShader cvs;
	FragmentShader cfs;
	if(!cvs.Load("shaders/persist-vertex.glsl") || !cfs.Load("shaders/persist-fragment.glsl"))
		LogFatal("failed to load persist shaders, aborting\n");

	m_persistProgram.Add(cvs);
	m_persistProgram.Add(cfs);
	if(!m_persistProgram.Link())
		LogFatal("failed to link shader program, aborting\n");

	//Create the VAO/VBO for a fullscreen polygon
	float verts[8] =
	{
		-1, -1,
		 1, -1,
		 1,  1,
		-1,  1
	};
	m_persistVBO.Bind();
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	m_persistVAO.Bind();
	m_persistProgram.EnableVertexArray("vert");
	m_persistProgram.SetVertexAttribPointer("vert", 2, 0);
}

void WaveformArea::InitializeCairoPass()
{
	//Set up shaders
	VertexShader cvs;
	FragmentShader cfs;
	if(!cvs.Load("shaders/cairo-vertex.glsl") || !cfs.Load("shaders/cairo-fragment.glsl"))
		LogFatal("failed to load cairo shaders, aborting\n");

	m_cairoProgram.Add(cvs);
	m_cairoProgram.Add(cfs);
	if(!m_cairoProgram.Link())
		LogFatal("failed to link shader program, aborting\n");

	//Create the VAO/VBO for a fullscreen polygon
	float verts[8] =
	{
		-1, -1,
		 1, -1,
		 1,  1,
		-1,  1
	};
	m_cairoVBO.Bind();
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	m_cairoVAO.Bind();
	m_cairoProgram.EnableVertexArray("vert");
	m_cairoProgram.SetVertexAttribPointer("vert", 2, 0);
}

bool WaveformArea::IsWaterfall()
{
	auto fall = dynamic_cast<WaterfallDecoder*>(m_channel);
	return (fall != NULL);
}

bool WaveformArea::IsAnalog()
{
	auto pdat = m_channel->GetData();
	return (dynamic_cast<AnalogCapture*>(pdat) != NULL);
}

bool WaveformArea::IsEye()
{
	auto eye = dynamic_cast<EyeDecoder2*>(m_channel);
	return (eye != NULL);
}

bool WaveformArea::IsFFT()
{
	return (m_channel->GetYAxisUnits().GetType() == Unit::UNIT_DB);
}

bool WaveformArea::IsTime()
{
	return (m_channel->GetYAxisUnits().GetType() == Unit::UNIT_PS);
}
