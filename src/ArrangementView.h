#pragma once

#include <gtkmm.h>
#include <Track.h>
#include <optional>

// One mono OPL voice per lane, sharing the same tick axis.
class ArrangementView : public Gtk::Box {
public:
	ArrangementView();
	~ArrangementView() override;
	void fit_song();
	void refresh();
	void clear_selection();
	bool select_note(int channel, int tick);
	void move_selected(int ticks, int semitones, int length_delta = 0);
	void erase_selected();
	sigc::signal<void()> signal_open_piano;
private:
	void on_draw(const std::shared_ptr<Cairo::Context>& cr, int width, int height);
	void on_pressed(int presses, double x, double y);
	void on_released(int presses, double x, double y);
	void on_motion(double x, double y);
	bool on_key(guint key, guint code, Gdk::ModifierType modifiers);
	bool on_scroll(double x, double y);
	void zoom(double multiplier);
	void update_hint();
	void apply_note(const std::optional<Note>& replacement);
	void mute_channel(int channel);
	void solo_channel(int channel);
	int channel_at(double y) const;
	int tick_at(double x) const;
	const Note* selected_note() const;
	int visible_channels() const;
	double row_height() const;
	Gtk::Box tools{Gtk::Orientation::HORIZONTAL, 6};
	Gtk::Button fit_button{"Fit song"}, zoom_out{"−"}, zoom_in{"+"};
	Gtk::Label hint;
	Gtk::DrawingArea canvas;
	Glib::RefPtr<Gtk::Adjustment> horizontal;
	Gtk::Scrollbar scrollbar;
	Glib::RefPtr<Gtk::GestureClick> click;
	Glib::RefPtr<Gtk::EventControllerScroll> scroll;
	bool fit_mode = true, dragging = false, resizing = false;
	double pixels_per_tick = 4.0, press_x = 0;
	int selected_channel = -1, selected_offset = -1, soloed_channel = -1;
	std::array<bool, 11> before_solo{};
	std::optional<Note> original, preview;
	std::vector<sigc::connection> subscriptions;
	static constexpr int labels_width = 188;
	static constexpr int ruler_height = 28;
};
