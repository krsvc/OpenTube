#include "headers.hpp"
#include "ui/colors.hpp"
#include "network_decoder/playback_diag.hpp"

bool err_error_display = false;
std::string err_error_summary = "N/A";
std::string err_error_description = "N/A";
std::string err_error_place = "N/A";
std::string err_error_code = "N/A";

bool Util_err_query_error_show_flag(void) { return err_error_display; }

void Util_err_set_error_message(std::string summary, std::string description, std::string place) {
	Util_err_set_error_message(summary, description, place, 1234567890);
}

void Util_err_set_error_message(std::string summary, std::string description, std::string place, int error_code) {
	char cache[128];
	Util_err_clear_error_message();
	err_error_summary = summary;
	err_error_description = description;
	err_error_place = place;
	if (error_code == 1234567890) {
		err_error_code = "N/A";
	} else {
		sprintf(cache, "0x%x", error_code);
		err_error_code = cache;
	}
}

void Util_err_set_error_show_flag(bool flag) { err_error_display = flag; }

void Util_err_clear_error_message(void) {
	err_error_summary = "N/A";
	err_error_description = "N/A";
	err_error_place = "N/A";
	err_error_code = "N/A";
}

void Util_err_save_error(void) {
	// redact urls / query strings / tokens before anything is persisted to the SD card
	std::string content = playback_diag::redact(err_error_summary) + "\n" +
	                      playback_diag::redact(err_error_description) + "\n" + playback_diag::redact(err_error_place) +
	                      "\n" + err_error_code;
	Path(DEF_MAIN_DIR + "error/" + std::to_string(var_years) + std::to_string(var_months) + std::to_string(var_days) +
	     std::to_string(var_minutes) + std::to_string(var_seconds) + ".txt")
	    .write_file((const u8 *)content.c_str(), content.length());
	Util_err_set_error_show_flag(false);
}

// error dialog buttons: the same rectangles are drawn and hit-tested
#define ERR_BUTTON_Y 150
#define ERR_BUTTON_HEIGHT 20
#define ERR_CLOSE_BUTTON_X 150
#define ERR_CLOSE_BUTTON_WIDTH 55
#define ERR_SAVE_BUTTON_X 209
#define ERR_SAVE_BUTTON_WIDTH 50
static bool err_in_button(const Hid_info &key, int x, int width) {
	return key.p_touch && key.touch_x >= x && key.touch_x < x + width && key.touch_y >= ERR_BUTTON_Y &&
	       key.touch_y < ERR_BUTTON_Y + ERR_BUTTON_HEIGHT;
}

void Util_err_main(Hid_info key) {
	if (key.p_a || err_in_button(key, ERR_CLOSE_BUTTON_X, ERR_CLOSE_BUTTON_WIDTH)) {
		err_error_display = false;
		var_need_refresh = true;
	} else if (key.p_x || err_in_button(key, ERR_SAVE_BUTTON_X, ERR_SAVE_BUTTON_WIDTH)) {
		Util_err_save_error();
		var_need_refresh = true;
	}
}

void Util_err_draw(void) {
	Draw_texture(var_square_image[0], POCKET_SCRIM, 0.0, 0.0, 320.0, 240.0);
	Draw_texture(var_square_image[0], POCKET_SEPARATOR, 19.0, 29.0, 282.0, 152.0);
	Draw_texture(var_square_image[0], POCKET_SURFACE, 20.0, 30.0, 280.0, 150.0);
	Draw_texture(var_square_image[0], POCKET_ACCENT, ERR_CLOSE_BUTTON_X, ERR_BUTTON_Y, ERR_CLOSE_BUTTON_WIDTH,
	             ERR_BUTTON_HEIGHT);
	Draw_texture(var_square_image[0], POCKET_PRESSED, ERR_SAVE_BUTTON_X, ERR_BUTTON_Y, ERR_SAVE_BUTTON_WIDTH,
	             ERR_BUTTON_HEIGHT);

	Draw("Summary : ", 22.5, 40.0, 0.45, 0.45, POCKET_ACCENT);
	Draw(err_error_summary, 22.5, 50.0, 0.45, 0.45, DEFAULT_TEXT_COLOR);
	Draw("Description : ", 22.5, 60.0, 0.45, 0.45, POCKET_ACCENT);
	Draw(err_error_description, 22.5, 70.0, 0.4, 0.4, DEFAULT_TEXT_COLOR);
	Draw("Place : ", 22.5, 90.0, 0.45, 0.45, POCKET_ACCENT);
	Draw(err_error_place, 22.5, 100.0, 0.45, 0.45, DEFAULT_TEXT_COLOR);
	Draw("Error code : ", 22.5, 110.0, 0.45, 0.45, POCKET_ACCENT);
	Draw(err_error_code, 22.5, 120.0, 0.45, 0.45, DEFAULT_TEXT_COLOR);
	Draw("Close (A)", 152.5, 152.5, 0.375, 0.375, POCKET_ON_ACCENT);
	Draw("Save (X)", 211.5, 152.5, 0.375, 0.375, DEFAULT_TEXT_COLOR);
}
