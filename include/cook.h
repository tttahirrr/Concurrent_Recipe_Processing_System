#ifndef COOK_H
#define COOK_H

#include <sys/types.h>
#include "cookbook.h"


void debug_print(COOKBOOK *cbp);

void init_work_queue();

void parse_command_line(int argc, char *argv[], char **cookbook_filename, int *max_cooks, char **main_recipe_name);

int perform_dependency_analysis(COOKBOOK *cbp, const char *main_recipe_name);

void process_recipes(COOKBOOK *cbp, int max_cooks);

void cleanup(COOKBOOK *cbp);

#endif
