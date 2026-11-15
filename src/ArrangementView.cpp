#include <ArrangementView.h>
#include <AdPlayer.h>
#include <ComposerPanel.h>
#include <CommonWidgets.h>
#include <algorithm>
#include <cmath>

using namespace Gtk;

ArrangementView::ArrangementView() : Box(Orientation::VERTICAL, 4),
	horizontal(Adjustment::create(0, 0, 100, 1, 16, 16)),
	scrollbar(horizontal, Orientation::HORIZONTAL) {
	set_name("arrangement");
	set_expand(true);
    tools.set_margin_start(8);
    tools.set_margin_end(8);
    tools.set_margin_top(6);
    tools.set_margin_bottom(2);
    fit_button.set_size_request(80, 30);
    zoom_out.set_size_request(32, 30);
    zoom_in.set_size_request(32, 30);
	tools.append(fit_button);
	tools.append(zoom_out);
	tools.append(zoom_in);
	hint.set_xalign(0);
	hint.set_hexpand(true);
	hint.set_ellipsize(Pango::EllipsizeMode::END);
	tools.append(hint);
	append(tools);
	canvas.set_expand(true);
	canvas.set_content_height(330);
	canvas.set_focusable(true);
	canvas.set_draw_func(sigc::mem_fun(*this, &ArrangementView::on_draw));
	canvas.set_tooltip_text("Click a note to select. Drag to move; drag its right edge to resize.\n"
		"Arrows: move by tick / semitone. Shift+Left/Right: resize. Delete: erase.\n"
		"Double-click: piano editor. M: mute lane. S: solo lane. Ruler: seek.\n"
		"Scroll: timeline. Ctrl+scroll: zoom. Fit song: show the complete arrangement.");
	append(canvas);
	append(scrollbar);
	fit_button.signal_clicked().connect(sigc::mem_fun(*this, &ArrangementView::fit_song));
	zoom_out.signal_clicked().connect([this] { zoom(1.0 / 1.5); });
	zoom_in.signal_clicked().connect([this] { zoom(1.5); });
	horizontal->signal_value_changed().connect([this] { canvas.queue_draw(); });
	click = GestureClick::create();
	click->set_button(GDK_BUTTON_PRIMARY);
	click->signal_pressed().connect(sigc::mem_fun(*this, &ArrangementView::on_pressed));
	click->signal_released().connect(sigc::mem_fun(*this, &ArrangementView::on_released));
	canvas.add_controller(click);
	auto motion = EventControllerMotion::create();
	motion->signal_motion().connect(sigc::mem_fun(*this, &ArrangementView::on_motion));
	canvas.add_controller(motion);
	auto keys = EventControllerKey::create();
	keys->signal_key_pressed().connect(sigc::mem_fun(*this, &ArrangementView::on_key), false);
	canvas.add_controller(keys);
	scroll = EventControllerScroll::create();
	scroll->set_flags(EventControllerScroll::Flags::BOTH_AXES);
	scroll->signal_scroll().connect(sigc::mem_fun(*this, &ArrangementView::on_scroll), false);
	canvas.add_controller(scroll);
	subscriptions.push_back(signal_track_changed.connect([this] {
		clear_selection();
		soloed_channel = -1;
		fit_song();
	}));
	subscriptions.push_back(signal_channel_changed.connect([this] {
		if (selected_channel != current_channel->channel_number) clear_selection();
		refresh();
	}));
	subscriptions.push_back(signal_bank_changed.connect(sigc::mem_fun(*this, &ArrangementView::refresh)));
	subscriptions.push_back(ComposerPanel::signal_content_changed.connect(sigc::mem_fun(*this, &ArrangementView::refresh)));
	subscriptions.push_back(ComposerPanel::signal_channel_controls_changed.connect(sigc::mem_fun(*this, &ArrangementView::refresh)));
	subscriptions.push_back(ComposerPanel::signal_cursor_moved.connect(sigc::mem_fun(*this, &ArrangementView::refresh)));
	subscriptions.push_back(ComposerPanel::signal_playback_changed.connect([this] {
		dragging = false;
		preview.reset();
		refresh();
	}));
	update_hint();
}

ArrangementView::~ArrangementView() {
	for (auto& subscription : subscriptions) subscription.disconnect();
}

int ArrangementView::visible_channels() const { return current_track->melodic_mode ? 9 : 11; }
double ArrangementView::row_height() const {
	return std::max(1.0, double(canvas.get_height() - ruler_height) / visible_channels());
}
int ArrangementView::channel_at(double y) const {
	if (y < ruler_height || y >= canvas.get_height()) return -1;
	return std::clamp(int((y - ruler_height) / row_height()), 0, visible_channels() - 1);
}
int ArrangementView::tick_at(double x) const {
	return std::clamp(int(std::floor(horizontal->get_value() + (x - labels_width) / pixels_per_tick)), 0, 65534);
}
const Note* ArrangementView::selected_note() const {
	if (selected_channel < 0 || selected_channel >= visible_channels()) return nullptr;
	const auto* note = current_track->get_channel(selected_channel)->get_note_on_tick(selected_offset);
	return note && note->offset == selected_offset ? note : nullptr;
}
void ArrangementView::update_hint() {
	if (adplayer && adplayer->is_playing()) {
		hint.set_text("Playing · pause to edit · M mute / S solo · ruler to seek");
	}
	else if (const auto* note = selected_note()) {
		hint.set_text("CH" + std::to_string(selected_channel + 1) + " · " + note_number_to_letter(note->pitch) +
			" · tick " + std::to_string(note->offset) + " · length " + std::to_string(note->length) +
			" · arrows move · Shift+arrows resize · Delete");
	}
	else hint.set_text("All channels · click/drag notes · double-click for piano · M mute / S solo");
}
void ArrangementView::refresh() {
	if (!fit_mode && adplayer && adplayer->is_playing()) {
		const double page = horizontal->get_page_size();
		if (cursor_tick < horizontal->get_value() || cursor_tick >= horizontal->get_value() + page) {
			horizontal->set_value(std::max(0.0, cursor_tick - page * 0.2));
		}
	}
	update_hint();
	canvas.queue_draw();
}
void ArrangementView::clear_selection() {
	selected_channel = selected_offset = -1;
	dragging = false;
	original.reset();
	preview.reset();
	refresh();
}
bool ArrangementView::select_note(int channel, int tick) {
	if (channel < 0 || channel >= visible_channels()) return false;
	const auto* note = current_track->get_channel(channel)->get_note_on_tick(tick);
	if (!note) { clear_selection(); return false; }
	ChannelButton::set_pressed_channel(channel);
	selected_channel = channel;
	selected_offset = note->offset;
	selection1 = selection2 = note->offset;
	if (!adplayer || !adplayer->is_playing()) ComposerPanel::set_cursor_tick(note->offset);
	refresh();
	return true;
}
void ArrangementView::fit_song() {
	fit_mode = true;
	horizontal->set_value(0);
	refresh();
}
void ArrangementView::zoom(double multiplier) {
	fit_mode = false;
	pixels_per_tick = std::clamp(pixels_per_tick * multiplier, 0.01, 80.0);
	refresh();
}
bool ArrangementView::on_scroll(double x, double y) {
	if (bool(scroll->get_current_event_state() & Gdk::ModifierType::CONTROL_MASK)) {
		zoom(y < 0 ? 1.25 : 1.0 / 1.25);
	}
	else horizontal->set_value(horizontal->get_value() + (x + y) * std::max(1.0, 48.0 / pixels_per_tick));
	return true;
}

void ArrangementView::mute_channel(int channel) {
	if (!adplayer) return;
	if (soloed_channel >= 0) {
		for (int i = 0; i < 11; ++i) adplayer->set_channel_enable(i, before_solo[i]);
	}
	soloed_channel = -1;
	adplayer->set_channel_enable(channel, !enabled_channels[channel]);
	ComposerPanel::signal_channel_controls_changed.emit();
	for (auto* button : ChannelButton::buttons) button->queue_draw();
}
void ArrangementView::solo_channel(int channel) {
	if (!adplayer) return;
	if (soloed_channel == channel) {
		for (int i = 0; i < 11; ++i) adplayer->set_channel_enable(i, before_solo[i]);
		soloed_channel = -1;
	}
	else {
		if (soloed_channel < 0) before_solo = enabled_channels;
		soloed_channel = channel;
		for (int i = 0; i < 11; ++i) adplayer->set_channel_enable(i, i == channel);
	}
	ComposerPanel::signal_channel_controls_changed.emit();
	for (auto* button : ChannelButton::buttons) button->queue_draw();
}

void ArrangementView::on_pressed(int presses, double x, double y) {
	canvas.grab_focus();
	if (y < ruler_height) {
		if (x >= labels_width) {
			const int tick = tick_at(x);
			selection1 = selection2 = tick;
			if (adplayer && adplayer->is_playing()) adplayer->seek(tick);
			else ComposerPanel::set_cursor_tick(tick);
		}
		return;
	}
	const int channel = channel_at(y);
	if (channel < 0) return;
	ChannelButton::set_pressed_channel(channel);
	if (x < 28) { mute_channel(channel); return; }
	if (x < 54) { solo_channel(channel); return; }
	if (x < labels_width) return;
	const int tick = tick_at(x);
	if (!select_note(channel, tick)) return;
	const auto* note = selected_note();
	if (presses == 2) { signal_open_piano.emit(); return; }
	if (!ComposerPanel::editing_allowed()) { refresh(); return; }
	original = preview = *note;
	press_x = x;
	dragging = true;
	const double end_x = labels_width + (note->get_end_offset() - horizontal->get_value()) * pixels_per_tick;
	resizing = note->length * pixels_per_tick >= 8 && end_x - x <= 6;
	refresh();
}
void ArrangementView::on_motion(double x, double) {
	if (!dragging || !original || !ComposerPanel::editing_allowed()) return;
	preview = original;
	const int delta = int(std::round((x - press_x) / pixels_per_tick));
	if (resizing) preview->length = std::clamp(original->length + delta, 1, 65535 - original->offset);
	else preview->offset = std::clamp(original->offset + delta, 0, 65535 - original->length);
	canvas.queue_draw();
}
void ArrangementView::on_released(int, double x, double y) {
	if (!dragging) return;
	on_motion(x, y);
	dragging = false;
	if (preview && original && (preview->offset != original->offset || preview->length != original->length)) apply_note(preview);
	preview.reset();
	original.reset();
	refresh();
}
void ArrangementView::apply_note(const std::optional<Note>& replacement) {
	if (!ComposerPanel::editing_allowed()) return;
	const auto* selected = selected_note();
	if (!selected) return;
	const Note old = *selected;
	ChannelButton::set_pressed_channel(selected_channel);
	// A whole-lane snapshot preserves split/overwritten neighbours exactly on undo.
	auto* undo = new UndoNotes(UndoCommand::MOVE_TICKS, UndoNotes::ERASE_OLD | UndoNotes::WRITE_NEW);
	undo->old_notes = current_channel->notes;
	undo->new_notes = undo->old_notes;
	undo->new_notes.erase(old.offset, old.length);
	if (replacement) undo->new_notes.add(*replacement);
	undo->redo();
	ComposerPanel::current_undo()->add_undo(undo);
	selected_offset = replacement ? replacement->offset : -1;
	selection1 = selection2 = replacement ? replacement->offset : old.offset;
	ComposerPanel::signal_content_changed.emit();
}
void ArrangementView::move_selected(int ticks, int semitones, int length_delta) {
	const auto* selected = selected_note();
	if (!selected || !ComposerPanel::editing_allowed()) return;
	Note next = *selected;
	next.offset = std::clamp(next.offset + ticks, 0, 65535 - next.length);
	next.pitch = std::clamp(next.pitch + semitones, 0, pitch_range - 1);
	next.length = std::clamp(next.length + length_delta, 1, 65535 - next.offset);
	if (next.offset != selected->offset || next.pitch != selected->pitch || next.length != selected->length) apply_note(next);
}
void ArrangementView::erase_selected() { apply_note(std::nullopt); }
bool ArrangementView::on_key(guint key, guint, Gdk::ModifierType modifiers) {
	const bool shift = bool(modifiers & Gdk::ModifierType::SHIFT_MASK);
	const bool ctrl = bool(modifiers & Gdk::ModifierType::CONTROL_MASK);
	const int step = bool(modifiers & Gdk::ModifierType::ALT_MASK) ? current_track->ticks_per_beat * current_track->beats_per_measure : 1;
	if (ctrl) return false; // Existing composer undo/save/copy shortcuts.
	switch (key) {
	case GDK_KEY_Left: move_selected(shift ? 0 : -step, 0, shift ? -step : 0); return true;
	case GDK_KEY_Right: move_selected(shift ? 0 : step, 0, shift ? step : 0); return true;
	case GDK_KEY_Up: move_selected(0, -1); return true;
	case GDK_KEY_Down: move_selected(0, 1); return true;
	case GDK_KEY_Delete: case GDK_KEY_BackSpace: erase_selected(); return true;
	case GDK_KEY_Escape: clear_selection(); return true;
	case GDK_KEY_m: case GDK_KEY_M: mute_channel(current_channel->channel_number); return true;
	case GDK_KEY_s: case GDK_KEY_S: solo_channel(current_channel->channel_number); return true;
	default: return false;
	}
}

void ArrangementView::on_draw(const std::shared_ptr<Cairo::Context>& cr, int width, int height) {
	if (!current_track || width <= labels_width || height <= ruler_height) return;
	const int total_ticks = std::max(1, current_track->get_tick_count());
	const int ticks_per_bar = std::max(1, int(current_track->ticks_per_beat) * current_track->beats_per_measure);
	if (fit_mode) pixels_per_tick = double(width - labels_width - 12) / std::max(total_ticks + 1, ticks_per_bar);
	const double page = (width - labels_width) / pixels_per_tick;
	const double upper = std::max(double(total_ticks + ticks_per_bar), page);
	if (horizontal->get_page_size() != page || horizontal->get_upper() != upper) {
		horizontal->configure(horizontal->get_value(), 0, upper, 1, ticks_per_bar, page);
	}
	const double start = horizontal->get_value();
	const double lane_height = row_height();
	cr->set_source_rgb(0.075, 0.09, 0.12);
	cr->paint();
	auto text = [&](const std::string& value, double x, double y, double size, double r, double g, double b) {
		cr->set_source_rgb(r, g, b);
		cr->set_font_size(size);
		cr->move_to(x, y);
		cr->show_text(value);
	};
	int stride = ticks_per_bar;
	while (stride * pixels_per_tick < 65) stride *= 2;
	for (int tick = int(start / stride) * stride; tick < start + page; tick += stride) {
		const double x = labels_width + (tick - start) * pixels_per_tick;
		if (x < labels_width) continue;
		cr->set_source_rgb(0.22, 0.25, 0.3);
		cr->set_line_width(1);
		cr->move_to(x, ruler_height);
		cr->line_to(x, height);
		cr->stroke();
		text(std::to_string(tick / ticks_per_bar + 1), x + 3, 18, 11, 0.7, 0.76, 0.82);
	}
	text("M    S     CHANNEL / INSTRUMENT", 6, 18, 10, 0.7, 0.76, 0.82);
	for (int channel = 0; channel < visible_channels(); ++channel) {
		auto& lane = *current_track->get_channel(channel);
		const double top = ruler_height + channel * lane_height;
		if (channel == current_channel->channel_number) {
			cr->set_source_rgba(0.25, 0.43, 0.65, 0.17);
			cr->rectangle(0, top, width, lane_height);
			cr->fill();
		}
		cr->set_source_rgb(0.2, 0.23, 0.28);
		cr->move_to(0, top + lane_height);
		cr->line_to(width, top + lane_height);
		cr->stroke();
		text("M", 9, top + lane_height / 2 + 4, 12, enabled_channels[channel] ? 0.45 : 1.0, 0.65, 0.6);
		text("S", 35, top + lane_height / 2 + 4, 12, soloed_channel == channel ? 1.0 : 0.45, 0.76, 0.5);
		std::string role = "CH" + std::to_string(channel + 1);
		if (!current_track->melodic_mode && channel >= 6) {
			const char* roles[] = {"Bass drum", "Snare", "Tom", "Cymbal", "Hi-hat"};
			role += " · " + std::string(roles[channel - 6]);
		}
		text(role, 60, top + lane_height * 0.43, 11, 0.87, 0.91, 0.96);
		int event_tick;
		std::string instrument;
		lane.get_last_instrument_event(cursor_tick, event_tick, instrument);
		if (instrument.size() > 17) instrument.resize(17);
		text(instrument, 60, top + lane_height * 0.78, 10, 0.55, 0.65, 0.74);
		cr->save();
		cr->rectangle(labels_width, top + 1, width - labels_width, lane_height - 2);
		cr->clip();
		for (const Note& stored : lane.notes) {
			const bool selected = channel == selected_channel && stored.offset == selected_offset;
			const Note& note = selected && preview ? *preview : stored;
			if (note.get_end_offset() < start || note.offset > start + page) continue;
			const double x = labels_width + (note.offset - start) * pixels_per_tick;
			const double note_height = std::max(4.0, lane_height * 0.19);
			const double y = top + 6 + double(note.pitch) / (pitch_range - 1) * std::max(1.0, lane_height - note_height - 12);
			cr->set_source_rgba(lane.color.get_red(), lane.color.get_green(), lane.color.get_blue(), enabled_channels[channel] ? 0.85 : 0.28);
			cr->rectangle(x, y, std::max(2.0, note.length * pixels_per_tick - 0.6), note_height);
			cr->fill_preserve();
			if (selected) {
				cr->set_source_rgb(1, 1, 1);
				cr->set_line_width(1.5);
				cr->stroke();
			}
			else cr->begin_new_path();
		}
		cr->restore();
	}
	const double cursor_x = labels_width + (cursor_tick - start) * pixels_per_tick;
	if (cursor_x >= labels_width && cursor_x <= width) {
		cr->set_source_rgb(1, 0.77, 0.28);
		cr->set_line_width(1.5);
		cr->move_to(cursor_x, 0);
		cr->line_to(cursor_x, height);
		cr->stroke();
	}
}
