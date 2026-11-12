#pragma once

namespace msgsub {

void reset();
void start_choice();
void start_number();
void start_item();
bool any_active();
void update();
void draw();

}

namespace msgwin {

float y();
float height();
float scale();
void update_placement();
void terminate();

}
