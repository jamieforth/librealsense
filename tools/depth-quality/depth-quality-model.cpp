#include <iomanip>
#include "depth-quality-model.h"
#include <librealsense2/rs_advanced_mode.hpp>
#include "model-views.h"

namespace rs2
{
    namespace depth_quality
    {
        tool_model::tool_model()
            : _update_readonly_options_timer(std::chrono::seconds(6)), _roi_percent(0.4f),
              _roi_located(std::chrono::seconds(4))
        {
            _viewer_model.is_3d_view = true;
            _viewer_model.allow_3d_source_change = false;
            _viewer_model.allow_stream_close = false;
            _viewer_model.draw_plane = true;
        }

        void tool_model::start(ux_window& window)
        {
            _pipe.enable_stream(RS2_STREAM_DEPTH, 0, 0, 0, RS2_FORMAT_Z16, 30);
            _pipe.enable_stream(RS2_STREAM_INFRARED, 0, 0, 0, RS2_FORMAT_RGB8, 30);

            // Wait till a valid device is found
            try {
                _pipe.start();
            }
            catch (...)
            {
                // Switch to infrared luminocity as a secondary in case synthetic chroma is not supported
                _pipe.disable_all();
                _pipe.enable_stream(RS2_STREAM_DEPTH, 0, 0, 0, RS2_FORMAT_Z16, 30);
                _pipe.enable_stream(RS2_STREAM_INFRARED, 1, 0, 0, RS2_FORMAT_Y8, 30);
                _pipe.start();
            }

            // Toggle advanced mode
            auto dev = _pipe.get_device();
            if (dev.is<rs400::advanced_mode>())
            {
                auto advanced_mode = dev.as<rs400::advanced_mode>();
                if (!advanced_mode.is_enabled())
                {
                    window.add_on_load_message("Toggling device to Advanced Mode...");
                    advanced_mode.toggle_advanced_mode(true);
                    std::this_thread::sleep_for(std::chrono::seconds(4)); // TODO: wait for connect event
                }
            }

            update_configuration();
        }

        void tool_model::draw_instructions(ux_window& win, const rect& viewer_rect)
        {
            _roi_located.add_value(is_valid(_metrics_model.get_plane()));
            if (!_roi_located.eval())
            {
                auto flags = ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoTitleBar;

                ImGui::PushStyleColor(ImGuiCol_Text, yellow);
                ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, white);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 5, 5 });
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, blend(sensor_bg, 0.8f));
                ImGui::SetNextWindowPos({ viewer_rect.w / 2 + viewer_rect.x - 225.f, viewer_rect.h / 2 + viewer_rect.y - 38.f });

                ImGui::SetNextWindowSize({ 450.f, 76.f });
                ImGui::Begin("Rect not detected window", nullptr, flags);

                ImGui::PushFont(win.get_large_font());
                ImGui::Text(u8"\n   \uf1b2  Please point the camera to a flat Wall / Surface!");
                ImGui::PopFont();

                ImGui::End();
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(2);
            }
        }

        void tool_model::render(ux_window& win)
        {
            win.begin_viewport();

            rect viewer_rect = { _viewer_model.panel_width,
                _viewer_model.panel_y, win.width() -
                _viewer_model.panel_width,
                win.height() - _viewer_model.panel_y };

            if (_first_frame)
            {
                _viewer_model.update_3d_camera(viewer_rect, win.get_mouse(), true);
                _first_frame = false;
            }

            _viewer_model.show_top_bar(win, viewer_rect);
            _viewer_model.roi_rect = _metrics_model.get_plane();
            _viewer_model.draw_viewport(viewer_rect, win, 1, _error_message);

            ImGui::PushStyleColor(ImGuiCol_WindowBg, button_color);
            ImGui::SetNextWindowPos({ 0, 0 });
            ImGui::SetNextWindowSize({ _viewer_model.panel_width, _viewer_model.panel_y });
            ImGui::Begin("Add Device Panel", nullptr, viewer_ui_traits::imgui_flags);
            ImGui::End();
            ImGui::PopStyleColor();

            // Set window position and size
            ImGui::SetNextWindowPos({ 0, _viewer_model.panel_y });
            ImGui::SetNextWindowSize({ _viewer_model.panel_width, win.height() - _viewer_model.panel_y });
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, sensor_bg);

            // *********************
            // Creating window menus
            // *********************
            ImGui::Begin("Control Panel", nullptr, viewer_ui_traits::imgui_flags | ImGuiWindowFlags_AlwaysVerticalScrollbar);
            ImGui::SetContentRegionWidth(_viewer_model.panel_width - 26);

            if (_device_model.get())
            {
                device_model* device_to_remove = nullptr;
                std::map<subdevice_model*, float> model_to_y;
                std::map<subdevice_model*, float> model_to_abs_y;
                auto windows_width = ImGui::GetContentRegionMax().x;

                _device_model->draw_controls(_viewer_model.panel_width, _viewer_model.panel_y,
                    win.get_font(), win.get_large_font(), win.get_mouse(),
                    _error_message, device_to_remove, _viewer_model, windows_width,
                    _update_readonly_options_timer,
                    model_to_y, model_to_abs_y);

                if (_depth_sensor_model.get())
                {
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, sensor_bg);
                    ImGui::PushStyleColor(ImGuiCol_Text, from_rgba(0xc3, 0xd5, 0xe5, 0xff));
                    ImGui::PushFont(win.get_font());

                    ImGui::PushStyleColor(ImGuiCol_Header, sensor_header_light_blue);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 10, 10 });
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, { 0, 0 });

                    if (ImGui::TreeNodeEx("Configuration", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::PopStyleVar();
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 2, 2 });

                        if (_depth_sensor_model->draw_stream_selection())
                        {
                            if (_depth_sensor_model->is_selected_combination_supported())
                            {
                                // Preserve streams and ui selections
                                auto primary = _depth_sensor_model->get_selected_profiles().front().as<video_stream_profile>();
                                auto secondary = _pipe.get_active_streams().back().as<video_stream_profile>();
                                _depth_sensor_model->store_ui_selection();

                                _pipe.stop();

                                _pipe.disable_all();

                                _pipe.enable_stream(primary.stream_type(), primary.stream_index(),
                                                    primary.width(), primary.height(), primary.format(), primary.fps());
                                _pipe.enable_stream(secondary.stream_type(), secondary.stream_index(),
                                                    primary.width(), primary.height(), secondary.format(), primary.fps());

                                // Wait till a valid device is found and responsive
                                bool success = false;
                                do
                                {
                                    try // Retries are needed to cope with HW stability issues
                                    {
                                        _pipe.start();
                                        success = true;
                                    }
                                    catch (...){}
                                } while (!success);

                                update_configuration();
                            }
                            else
                            {
                                _error_message = "Selected configuration is not supported!";
                                _depth_sensor_model->restore_ui_selection();
                            }
                        }

                        auto col0 = ImGui::GetCursorPosX();
                        auto col1 = 145.f;

                        ImGui::Text("Region of Interest:");
                        ImGui::SameLine(); ImGui::SetCursorPosX(col1);

                        ImGui::PushItemWidth(-1);
                        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, { 1,1,1,1 });

                        static std::vector<std::string> items{ "80%", "40%", "20%" };
                        if (draw_combo_box("##ROI Percent", items, _roi_combo_index))
                        {
                            if (_roi_combo_index == 0) _roi_percent = 0.8f;
                            else if (_roi_combo_index == 1) _roi_percent = 0.4f;
                            else if (_roi_combo_index == 2) _roi_percent = 0.2f;
                            update_configuration();
                        }

                        ImGui::PopStyleColor();
                        ImGui::PopItemWidth();
                        ImGui::SetCursorPosX(col0);

                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);
                        ImGui::TreePop();
                    }

                    ImGui::PopStyleVar();
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, { 0, 0 });

                    if (ImGui::TreeNodeEx("Metrics", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::PopStyleVar();
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 2, 2 });

                        _metrics_model.render(win);

                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);
                        ImGui::TreePop();
                    }

                    ImGui::PopStyleVar();
                    ImGui::PopStyleVar();
                    ImGui::PopFont();
                    ImGui::PopStyleColor(3);
                }
            }

            ImGui::PushStyleColor(ImGuiCol_Text, light_grey);
            ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, white);
            ImGui::Dummy({ 20,25 }); ImGui::SameLine();
            if (ImGui::Button(u8"\uf0c7 Save Report", { 140, 25 }))
            {
                snapshot_metrics();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Save Metrics snapshot. This will create:\nPNG image with the depth frame\nPLY 3D model with the point cloud\nJSON file with camera settings you can load later\nand a CSV with metrics recent values");
            }
            ImGui::PopStyleColor(2);

            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            frameset f;
            if (_pipe.poll_for_frames(&f))
            {
                for (auto&& frame : f)
                {
                    if (frame.is<depth_frame>() && !_viewer_model.paused)
                    {
                        _metrics_model.begin_process_frame(frame);
                    }
                    _viewer_model.upload_frame(std::move(frame));
                }
            }

            draw_instructions(win, viewer_rect);

            _viewer_model.gc_streams();
            _viewer_model.popup_if_error(win.get_font(), _error_message);
        }

        void tool_model::update_configuration()
        {
            // Capture the old configuration before reconfiguring the stream
            bool save = false;
            subdevice_ui_selection prev_ui;

            if (_depth_sensor_model)
            {
                prev_ui = _depth_sensor_model->last_valid_ui;
                save = true;
            }

            auto dev = _pipe.get_device();
            auto dpt_sensor = dev.first<depth_sensor>();
            _device_model = std::shared_ptr<rs2::device_model>(new device_model(dev, _error_message, _viewer_model));
            _device_model->allow_remove = false;
            _device_model->show_depth_only = true;
            _device_model->show_stream_selection = false;
            _depth_sensor_model = std::shared_ptr<rs2::subdevice_model>(
                new subdevice_model(dev, dpt_sensor, _error_message));
            _depth_sensor_model->draw_streams_selector = false;
            _depth_sensor_model->draw_fps_selector = true;

            // Retrieve stereo baseline for supported devices
            auto baseline_mm = -1.f;
            auto profiles = dpt_sensor.get_stream_profiles();
            auto right_sensor = std::find_if(profiles.begin(), profiles.end(), [](rs2::stream_profile& p)
            { return (p.stream_index() == 2) && (p.stream_type() == RS2_STREAM_INFRARED); });

            if (right_sensor != profiles.end())
            {
                auto left_sensor = std::find_if(profiles.begin(), profiles.end(), [](rs2::stream_profile& p)
                                    { return (p.stream_index() == 0) && (p.stream_type() == RS2_STREAM_DEPTH); });
                try
                {
                    auto extrin = (*left_sensor).get_extrinsics_to(*right_sensor);
                    baseline_mm = fabs(extrin.translation[0])*1000;  // baseline in mm
                }
                catch (...) {
                    _error_message = "Extrinsic parameters are not available";
                }
            }

            _metrics_model.reset();

            // Restore GUI controls to the selected configuration
            if (save)
            {
                _depth_sensor_model->ui = _depth_sensor_model->last_valid_ui = prev_ui;
            }

            // Connect the device_model to the viewer_model
            for (auto&& sub : _device_model.get()->subdevices)
            {
                if (!sub->s.is<depth_sensor>()) continue;

                sub->show_algo_roi = true;
                auto profiles = _pipe.get_active_streams();
                sub->streaming = true;      // The streaming activated externally to the device_model
                for (auto&& profile : profiles)
                {
                    _viewer_model.streams[profile.unique_id()].dev = sub;

                    if (profile.stream_type() == RS2_STREAM_DEPTH)
                    {
                        auto depth_profile = profile.as<video_stream_profile>();
                        _metrics_model.update_stream_attributes(depth_profile.get_intrinsics(),
                                                          sub->s.as<depth_sensor>().get_depth_scale(), baseline_mm);

                        _metrics_model.update_frame_attributes({ int(depth_profile.width() * (0.5f - 0.5f*_roi_percent)),
                                                           int(depth_profile.height() * (0.5f - 0.5f*_roi_percent)),
                                                           int(depth_profile.width() * (0.5f + 0.5f*_roi_percent)),
                                                           int(depth_profile.height() * (0.5f + 0.5f*_roi_percent)) });
                    }
                }

                sub->algo_roi = _metrics_model.get_roi();
            }
        }

        bool metric_plot::has_trend(bool positive)
        {
            const auto window_size = 110;
            const auto curr_window = 10;
            auto best = ranges[GREEN_RANGE].x;
            if (ranges[RED_RANGE].x < ranges[GREEN_RANGE].x)
                best = ranges[GREEN_RANGE].y;

            auto min_val = 0.f;
            for (int i = 0; i < curr_window; i++)
            {
                auto val = fabs(best - _vals[(SIZE + _idx - i) % SIZE]);
                min_val += val / curr_window;
            }

            auto improved = 0;
            for (int i = curr_window; i <= window_size; i++)
            {
                auto val = fabs(best - _vals[(SIZE + _idx - i) % SIZE]);
                if (positive && min_val < val * 0.8) improved++;
                if (!positive && min_val * 0.8 > val) improved++;
            }
            return improved > window_size * 0.4;
        }


        void tool_model::snapshot_metrics()
        {
            if (auto ret = file_dialog_open(save_file, NULL, NULL, NULL))
            {
                std::string filename_base(ret);

                // Save depth/ir images
                for (auto const &stream : _viewer_model.streams)
                {
                    if (auto frame = stream.second.texture->get_last_frame().as<video_frame>())
                    {
                        // Use the colorizer to get an rgb image for the depth stream
                        if (frame.is<rs2::depth_frame>())
                        {
                            rs2::colorizer color_map;
                            frame = color_map(frame);
                        }

                        std::string stream_desc = rs2_stream_to_string(frame.get_profile().stream_type());
                        std::string filename = filename_base + "_" + stream_desc + ".png";
                        save_to_png(filename.data(), frame.get_width(), frame.get_height(), frame.get_bytes_per_pixel(), frame.get_data(), frame.get_width() * frame.get_bytes_per_pixel());

                        _viewer_model.not_model.add_notification({ to_string() << stream_desc << " snapshot was saved to " << filename,
                            0, RS2_LOG_SEVERITY_INFO,
                            RS2_NOTIFICATION_CATEGORY_UNKNOWN_ERROR });
                    }
                }

                // Export 3d view in PLY format
                frame ply_texture;
                if (_viewer_model.selected_tex_source_uid >= 0)
                {
                    ply_texture = _viewer_model.streams[_viewer_model.selected_tex_source_uid].texture->get_last_frame();
                    if (ply_texture)
                        _viewer_model.pc.update_texture(ply_texture);
                }
                export_to_ply(filename_base + "_3d_mesh.ply", _viewer_model.not_model, _viewer_model.pc.get_points(), ply_texture);

                // Save Metrics
                _metrics_model.serialize_to_csv(filename_base + "_depth_metrics.csv");

                // Save camera configuration - supported when camera is in advanced mode only
                if (_device_model.get())
                {
                    if (auto adv = _device_model->dev.as<rs400::advanced_mode>())
                    {
                        std::string filename = filename_base + "_configuration.json";
                        std::ofstream out(filename);
                        out << adv.serialize_json();
                        out.close();
                    }
                }
            }
        }

        metrics_model::metrics_model() :
            _frame_queue(1),
            _depth_scale_units(0.f), _active(true)
        {
            _worker_thread = std::thread([this]() {
                while (_active)
                {
                    rs2::frame depth_frame;
                    if (!_frame_queue.poll_for_frame(&depth_frame))
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }

                    auto stream_type = depth_frame.get_profile().stream_type();

                    if (RS2_STREAM_DEPTH == stream_type)
                    {
                        float su = 0, baseline = -1.f;
                        rs2_intrinsics intrin;
                        region_of_interest roi;
                        {
                            std::lock_guard<std::mutex> lock(_m);
                            su = _depth_scale_units;
                            baseline = _stereo_baseline_mm;
                            intrin = _depth_intrinsic;
                            roi = _roi;
                        }

                        auto metrics = analyze_depth_image(depth_frame, su, baseline, &intrin, roi, callback);

                        {
                            std::lock_guard<std::mutex> lock(_m);
                            _latest_metrics = metrics;
                        }
                    }

                    // Artificially slow down the calculation, so even on small ROIs / resolutions
                    // the output is updated within reasonable interval (keeping it human readable)
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                }
            });
        }

        metrics_model::~metrics_model()
        {
            _active = false;
            _worker_thread.join();
            reset();
        }

        std::shared_ptr<metric_plot> tool_model::make_metric(
            const std::string& name, float min, float max,
            const std::string& units,
            const std::string& description)
        {
            auto res = std::make_shared<metric_plot>(name, min, max, units, description);
            _metrics_model.add_metric(res);
            return res;
        }

        void metric_plot::render(ux_window& win)
        {
            std::lock_guard<std::mutex> lock(_m);
            std::stringstream ss;
            auto val = _vals[(SIZE + _idx - 1) % SIZE];

            ss << _label << std::setprecision(2) << std::fixed  << std::setw(3) << val << " " << _units;

            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, sensor_bg);

            auto range = get_range(val);
            if (range == GREEN_RANGE)
                ImGui::PushStyleColor(ImGuiCol_Text, green);
            else if (range == YELLOW_RANGE)
                ImGui::PushStyleColor(ImGuiCol_Text, yellow);
            else if (range == RED_RANGE)
                ImGui::PushStyleColor(ImGuiCol_Text, redish);
            else
                ImGui::PushStyleColor(ImGuiCol_Text, from_rgba(0xc3, 0xd5, 0xe5, 0xff));

            ImGui::PushFont(win.get_font());

            ImGui::PushStyleColor(ImGuiCol_Header, sensor_header_light_blue);

            const auto left_x = 295.f;
            const auto indicator_flicker_rate = 200;
            auto alpha_value = static_cast<float>(fabs(sin(_model_timer.elapsed_ms() / indicator_flicker_rate)));

            _trending_up.add_value(has_trend(true));
            _trending_down.add_value(has_trend(false));

            if (_trending_up.eval())
            {
                auto color = blend(green, alpha_value);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                auto col0 = ImGui::GetCursorPos();
                ImGui::SetCursorPosX(left_x);
                ImGui::PushFont(win.get_large_font());
                ImGui::Text(u8"\uf102");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("This metric shows positive trend");
                }
                ImGui::PopFont();
                ImGui::SameLine(); ImGui::SetCursorPos(col0);
                ImGui::PopStyleColor();
            }
            else if (_trending_down.eval())
            {
                auto color = blend(redish, alpha_value);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                auto col0 = ImGui::GetCursorPos();
                ImGui::SetCursorPosX(left_x);
                ImGui::PushFont(win.get_large_font());
                ImGui::Text(u8"\uf103");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("This metric shows negative trend");
                }
                ImGui::PopFont();
                ImGui::SameLine(); ImGui::SetCursorPos(col0);
                ImGui::PopStyleColor();
            }
            else
            {
                auto col0 = ImGui::GetCursorPos();
                ImGui::SetCursorPosX(left_x);
                ImGui::PushFont(win.get_large_font());
                ImGui::Text(" ");
                ImGui::PopFont();
                ImGui::SameLine(); ImGui::SetCursorPos(col0);
            }

            if (ImGui::TreeNode(_label.c_str(), "%s", ss.str().c_str()))
            {
                ImGui::PushStyleColor(ImGuiCol_FrameBg, device_info_color);
                ImGui::PushStyleColor(ImGuiCol_Text, light_grey);
                ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, regular_blue);
                std::string did = to_string() << _id << "-desc";
                ImVec2 desc_size = { 270, 50 };
                auto lines = std::count(_description.begin(), _description.end(), '\n') + 1;
                desc_size.y = lines * 20.f;
                ImGui::InputTextMultiline(did.c_str(), const_cast<char*>(_description.c_str()),
                    _description.size() + 1, desc_size, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_ReadOnly);
                ImGui::PopStyleColor(3);

                ImGui::PlotLines(_id.c_str(), _vals, 100, _idx, ss.str().c_str(), _min, _max, { 270, 50 });

                ImGui::PushStyleColor(ImGuiCol_Text, green);
                ImGui::Text("[%.2f - %.2f] Pass", ranges[0].x, ranges[0].y);
                ImGui::PopStyleColor();

                ImGui::TreePop();
            }

            ImGui::PopFont();
            ImGui::PopStyleColor(3);
        }

        void metrics_model::render(ux_window& win)
        {
            for (auto&& plot : _plots)
            {
                plot->render(win);
            }
        }

        void metrics_model::serialize_to_csv(const std::string& filename) const
        {
            // RAII
            std::ofstream csv;

            csv.open(filename);

            // Create header line
            for (auto&& plot : _plots)
            {
                csv << plot->_name << ",";
            }
            csv << std::endl;
            for (size_t i = 0; i < metric_plot::SIZE; i++)
            {
                for (auto&& plot : _plots)
                {
                    csv << plot->_vals[(plot->_idx + i) % metric_plot::SIZE] << ",";
                }
                csv << std::endl;
            }

            csv.close();
        }
    }
}