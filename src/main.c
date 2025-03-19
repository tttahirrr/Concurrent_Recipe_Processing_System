#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "cookbook.h"
#include "cook.h"


int main(int argc, char *argv[]) {
    COOKBOOK *cbp;
    int err = 0;
    char *cookbook_filename = NULL;
    int max_cooks = 1;
    char *main_recipe_name = NULL;
    FILE *in;

    // call the function with command line arguments
    parse_command_line(argc, argv, &cookbook_filename, &max_cooks, &main_recipe_name);

    // open the cookbook file
    if ((in = fopen(cookbook_filename, "r")) == NULL)
    {
       fprintf(stderr, "Can't open cookbook '%s': %s\n", cookbook_filename, strerror(errno));
       exit(EXIT_FAILURE);
    }

    // parse the cookbook
    cbp = parse_cookbook(in, &err);
    fclose(in); // close the file after parsing
    if (err)
    {
       fprintf(stderr, "Error parsing cookbook '%s'\n", cookbook_filename);
       exit(EXIT_FAILURE);
    }

    // if main_recipe_name is NULL, set it to the name of the first recipe in the cookbook
    if (main_recipe_name == NULL)
    {
        if (cbp->recipes != NULL)
        {
           main_recipe_name = cbp->recipes->name;
        }
        else
        {
           fprintf(stderr, "Error: Cookbook '%s' contains no recipes\n", cookbook_filename);
           exit(EXIT_FAILURE);
        }
    }

    // initialize the work queue to manage recipes ready for processing
    init_work_queue();

    // do an analysis to determine all sub-recipes required by the main recipe
    if (perform_dependency_analysis(cbp, main_recipe_name) != 0) {
       fprintf(stderr, "Error during dependency analysis\n");
       exit(EXIT_FAILURE);
    }

    // call process_recipes to start the main processing loop
    process_recipes(cbp, max_cooks);

    // after processing, clean up resources before exiting
    cleanup(cbp);

    return 0;
}
