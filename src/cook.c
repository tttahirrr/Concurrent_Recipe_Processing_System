#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "cookbook.h"


//////////////////////////// header stuff ////////////////////////////

// structure to hold the state of each recipe
typedef struct recipe_state {
   int required;       // indicates if the recipe is required for the main recipe
   int processing;     // indicates if processing has started for this recipe
   int completed;      // indicates if the recipe has been completed successfully
   int failed;         // indicates if the recipe has failed
   pid_t pid;          // process ID of the cook process handling this recipe
} RECIPE_STATE;


typedef struct work_queue_node
{
   RECIPE *recipe;
   struct work_queue_node *next;
} WORK_QUEUE_NODE;


WORK_QUEUE_NODE *work_queue_head;
WORK_QUEUE_NODE *work_queue_tail;

COOKBOOK *cookbook_global;
char *main_recipe_name_global;
int active_cooks = 0; // # of active cook processes
int max_cooks_global = 1; // max cooks allowed
sigset_t mask_all, mask_sigchld, prev_mask; // signal masks for syncronization

void parse_command_line(int argc, char *argv[], char **cookbook_filename, int *max_cooks, char **main_recipe_name);
void enqueue_recipe(RECIPE *recipe);
RECIPE *dequeue_recipe();
int is_work_queue_empty();
void process_recipe(RECIPE *recipe);
int execute_task(TASK *task);
void sigchld_handler(int signo);
int mark_required_recipes(RECIPE *recipe);
int is_recipe_ready(RECIPE *recipe);
RECIPE *find_recipe_by_pid(pid_t pid);
RECIPE *find_recipe_by_name(COOKBOOK *cbp, const char *name);

//////////////////////////// header stuff ////////////////////////////


/*
handle the optional arguments
   -f cookbook:
       specifies the file containing the cookbook (.ckb file).
       if omitted, the default is cookbook.ckb in the current directory.

   -c max_cooks:
       specifies the maximum number of cooks (parallel workers).
       if omitted, the default is 1.

   main_recipe_name:
       specifies the main recipe to prepare.
       if omitted, the first recipe in the cookbook is used as the main recipe.
if invalid options are provided, display usage information & exit.
*/
void parse_command_line(int argc, char *argv[], char **cookbook_filename, int *max_cooks, char **main_recipe_name)
{
   // set default values
   *cookbook_filename = "cookbook.ckb"; // default cookbook filename
   *max_cooks = 1;                      // default max cooks
   *main_recipe_name = NULL;            // default main recipe name (use the first recipe if not provided)

   // index variable for looping through argv
   int i = 1;

   // loop through command line arguments
   while (i < argc)
   {
       char *arg = argv[i];


       // check if the argument is an option (starts with '-')
       if (arg[0] == '-')
       {
           // check which option it is
           if (strcmp(arg, "-f") == 0)
           {
               // make sure there is a next argument for the filename
               if (i + 1 < argc)
               {
                   *cookbook_filename = argv[++i]; // increment i & assign the filename
               }
               else
               {
                   fprintf(stderr, "Error: -f option requires a filename argument\n");
                   fprintf(stderr, "Usage: cook [-f cookbook] [-c max_cooks] [main_recipe_name]\n");
                   exit(EXIT_FAILURE);
               }
           }
           else if (strcmp(arg, "-c") == 0)
           {
               // make sure there is a next argument for the max cooks
               if (i + 1 < argc)
               {
                   *max_cooks = atoi(argv[++i]); // increment i & assign the max cooks
                   if (*max_cooks <= 0)
                   {
                       fprintf(stderr, "Error: -c option requires a positive integer\n");
                       exit(EXIT_FAILURE);
                   }
               }
               else
               {
                   fprintf(stderr, "Error: -c option requires a number argument\n");
                   fprintf(stderr, "Usage: cook [-f cookbook] [-c max_cooks] [main_recipe_name]\n");
                   exit(EXIT_FAILURE);
               }
           }
           else
           {
               // unknown option
               fprintf(stderr, "Error: Unknown option '%s'\n", arg);
               fprintf(stderr, "Usage: cook [-f cookbook] [-c max_cooks] [main_recipe_name]\n");
               exit(EXIT_FAILURE);
           }
       }
       else
       {
           // not an option. treat it as the main recipe name
           if (*main_recipe_name == NULL)
           {
               *main_recipe_name = arg;
           }
           else
           {
               // multiple main recipe names provided
               fprintf(stderr, "Error: Multiple main recipe names provided ('%s' and '%s')\n", *main_recipe_name, arg);
               fprintf(stderr, "Usage: cook [-f cookbook] [-c max_cooks] [main_recipe_name]\n");
               exit(EXIT_FAILURE);
           }
       }
       i++; // move to the next argument
   }
}


/*
   find the main recipe. start from the main recipe & identify all sub-recipes required
   initialize state for all recipes
   recursively mark required recipes starting from the main recipe
   enqueue leaf recipes into the work queue

   search for the main recipe by name or default to the first recipe.

   allocate a RECIPE_STATE structure for each recipe to track
   its status throughout execution.

   recursively mark all recipes required by the main recipe.
   involves traversing this_depends_on links.
   */
int perform_dependency_analysis(COOKBOOK *cbp, char *main_recipe_name)
{
   RECIPE *main_recipe = NULL;

   cookbook_global = cbp;
   main_recipe_name_global = main_recipe_name;

   // find the main recipe by name
   main_recipe = find_recipe_by_name(cbp, main_recipe_name);
   if (main_recipe == NULL)
   {
       fprintf(stderr, "Error: Main recipe '%s' not found in cookbook\n", main_recipe_name);
       return -1;
   }

   // initialize state for all recipes
   for (RECIPE *rp = cbp->recipes; rp != NULL; rp = rp->next)
   {
       RECIPE_STATE *state = calloc(1, sizeof(RECIPE_STATE));
       if (state == NULL)
       {
           perror("calloc");
           return -1;
       }
       rp->state = state;
   }

   // recursively mark required recipes starting from the main recipe
   if (mark_required_recipes(main_recipe) != 0)
   {
       return -1;
   }


   // enqueue leaf recipes (required recipes with no dependencies)
   for (RECIPE *rp = cbp->recipes; rp != NULL; rp = rp->next)
   {
       RECIPE_STATE *state = (RECIPE_STATE *)rp->state;
       if (state->required && rp->this_depends_on == NULL)
       {
           enqueue_recipe(rp);
       }
   }

   return 0;
}

int is_work_queue_empty()
{
    return (work_queue_head == NULL);
}


// returns the RECIPE pointer if found. otherwise returns NULL
RECIPE *find_recipe_by_name(COOKBOOK *cbp, const char *name)
{
   for (RECIPE *rp = cbp->recipes; rp != NULL; rp = rp->next)
   {
       if (strcmp(rp->name, name) == 0)
       {
           return rp;
       }
   }
   return NULL;
}


/*
   base case: if already required, return
   mark recipe as required
   recursively mark all sub-recipes
*/
int mark_required_recipes(RECIPE *recipe)
{
   RECIPE_STATE *state = (RECIPE_STATE *)recipe->state;


   if (state->required)
   {
       // already marked as required. avoid infinite recursion
       return 0;
   }


   state->required = 1; // mark as required


   // recursively mark all sub-recipes
   for (RECIPE_LINK *link = recipe->this_depends_on; link != NULL; link = link->next)
   {
       RECIPE *sub_recipe = link->recipe;
       if (sub_recipe == NULL)
       {
           fprintf(stderr, "Error: Recipe '%s' depends on non-existent recipe '%s'\n", recipe->name, link->name);
           return -1;
       }
       if (mark_required_recipes(sub_recipe) != 0)
       {
           return -1;
       }
   }


   return 0;
}


void init_work_queue()
{
   work_queue_head = NULL;
   work_queue_tail = NULL;
}


void enqueue_recipe(RECIPE *recipe)
{
   WORK_QUEUE_NODE *node = malloc(sizeof(WORK_QUEUE_NODE));
   if (node == NULL)
   {
       perror("malloc");
       exit(EXIT_FAILURE);
   }
   node->recipe = recipe;
   node->next = NULL;


   if (work_queue_tail == NULL)
   {
       // queue is empty
       work_queue_head = node;
   }
   else
   {
       work_queue_tail->next = node;
   }
   work_queue_tail = node;
}


RECIPE *dequeue_recipe() {
   if (work_queue_head == NULL) {
       return NULL;
   }

   WORK_QUEUE_NODE *node = work_queue_head;
   RECIPE *recipe = node->recipe;
   work_queue_head = node->next;
   if (work_queue_head == NULL) {
       work_queue_tail = NULL;
   }
   free(node);
   return recipe;
}


void debug_print(COOKBOOK *cbp){
 // testing: print recipes marked as required
   printf("Recipes marked as required:\n");
   for (RECIPE *rp = cbp->recipes; rp != NULL; rp = rp->next) {
       RECIPE_STATE *state = (RECIPE_STATE *)rp->state;
       if (state->required) {
           printf(" - %s\n", rp->name);
       }
   }

   // testing: print recipes in the work queue (leaf recipes)
   printf("\nRecipes in the work queue (leaf recipes):\n");
   WORK_QUEUE_NODE *current = work_queue_head;
   while (current != NULL) {
       printf(" - %s\n", current->recipe->name);
       current = current->next;
   }


   // clean up the work queue to prevent memory leaks
   while (work_queue_head != NULL) {
       dequeue_recipe();
   }
}

// "main processing loop"
void process_recipes(COOKBOOK *cbp, int max_cooks)
{
   // set the global max_cooks variable
   max_cooks_global = max_cooks;


   // set up signal handling for SIGCHLD
   struct sigaction sa;
   sa.sa_handler = sigchld_handler;
   sigemptyset(&sa.sa_mask); // no additional signals to block in handler
   sa.sa_flags = SA_RESTART; // restart interrupted system calls
   if (sigaction(SIGCHLD, &sa, NULL) == -1)
   {
       perror("sigaction");
       exit(EXIT_FAILURE);
   }


   // initialize signal masks
   sigfillset(&mask_all);             // mask all signals
   sigemptyset(&mask_sigchld);        // empty mask
   sigaddset(&mask_sigchld, SIGCHLD); // add SIGCHLD to the mask


   // main processing loop
   while (1)
   {
       // block SIGCHLD signals
       sigprocmask(SIG_BLOCK, &mask_sigchld, &prev_mask);


       // check if processing is complete
       if (is_work_queue_empty() && active_cooks == 0)
       {
           // all recipes have been processed
           sigprocmask(SIG_SETMASK, &prev_mask, NULL); // restore previous mask
           break;
       }


       // start new cook processes if possible
       if (!is_work_queue_empty() && active_cooks < max_cooks_global)
       {
           RECIPE *recipe = dequeue_recipe();
           if (recipe != NULL)
           {
               // start a new cook process
               pid_t pid = fork();
               if (pid == -1)
               {
                   perror("fork");
                   // re-enqueue the recipe if the fork failed
                   enqueue_recipe(recipe);
               }
               else if (pid == 0)
                {
                    // child process (cook process)

                    // unblock signals
                    sigprocmask(SIG_SETMASK, &prev_mask, NULL);

                    // reset SIGCHLD handler to default in the cook process
                    struct sigaction sa_default;
                    sa_default.sa_handler = SIG_DFL;
                    sigemptyset(&sa_default.sa_mask);
                    sa_default.sa_flags = 0;
                    if (sigaction(SIGCHLD, &sa_default, NULL) == -1)
                    {
                        perror("sigaction");
                        exit(EXIT_FAILURE);
                    }

                    // now proceed to process the recipe
                    process_recipe(recipe);

                    // exit with status based on recipe success or failure
                    RECIPE_STATE *state = (RECIPE_STATE *)recipe->state;
                    exit(state->failed ? EXIT_FAILURE : EXIT_SUCCESS);
                }

               else
               {
                   // parent process
                   // update recipe state
                   RECIPE_STATE *state = (RECIPE_STATE *)recipe->state;
                   state->processing = 1;
                   state->pid = pid;
                   active_cooks++;
               }
           }
       }
       else
       {
           // wait for a cook process to terminate
           sigsuspend(&prev_mask); // wait with previous mask (signals unblocked)
       }

       // unblock SIGCHLD signals
       sigprocmask(SIG_SETMASK, &prev_mask, NULL);
   }

   // after processing, check if the main recipe completed successfully
   // & set the exit status
   RECIPE *main_recipe = find_recipe_by_name(cbp, main_recipe_name_global);
   RECIPE_STATE *main_state = (RECIPE_STATE *)main_recipe->state;
   if (main_state->failed) {
       exit(EXIT_FAILURE);
   } else {
       exit(EXIT_SUCCESS);
   }
}


void sigchld_handler(int signo)
{
   pid_t pid;
   int status;


   // reap all terminated child processes
   while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
   {
       // find the recipe corresponding to this PID
       RECIPE *recipe = find_recipe_by_pid(pid);
       if (recipe == NULL)
       {
           // unknown child process. possibly a step in a pipeline
           continue;
       }


       RECIPE_STATE *state = (RECIPE_STATE *)recipe->state;
       if (WIFEXITED(status))
       {
           int exit_status = WEXITSTATUS(status);
           if (exit_status == 0)
           {
               // recipe completed successfully
               state->completed = 1;
           }
           else
           {
               // recipe failed
               state->failed = 1;
           }
       }
       else if (WIFSIGNALED(status))
       {
           // recipe process terminated by a signal
           state->failed = 1;
       }
       state->processing = 0;
       state->pid = 0;
       active_cooks--;


       // enqueue dependent recipes if they are now ready
       for (RECIPE_LINK *link = recipe->depend_on_this; link != NULL; link = link->next)
       {
           RECIPE *dependent_recipe = link->recipe;
           if (is_recipe_ready(dependent_recipe))
           {
               enqueue_recipe(dependent_recipe);
           }
       }
   }
}


int is_recipe_ready(RECIPE *recipe) {
   RECIPE_STATE *state = (RECIPE_STATE *)recipe->state;
   if (!state->required || state->processing || state->completed || state->failed) {
       return 0; // not ready
   }
   // check if all dependencies are completed successfully
   for (RECIPE_LINK *link = recipe->this_depends_on; link != NULL; link = link->next) {
       RECIPE *dep_recipe = link->recipe;
       RECIPE_STATE *dep_state = (RECIPE_STATE *)dep_recipe->state;
       if (!dep_state->completed) {
           return 0; // dependency not completed
       }
   }
   return 1; // ready to be processed
}


RECIPE *find_recipe_by_pid(pid_t pid) {
   // iterate over all recipes to find the one with the matching PID
   for (RECIPE *rp = cookbook_global->recipes; rp != NULL; rp = rp->next) {
       RECIPE_STATE *state = (RECIPE_STATE *)rp->state;
       if (state->pid == pid) {
           return rp;
       }
   }
   return NULL; // recipe not found
}


void process_recipe(RECIPE *recipe) {
   RECIPE_STATE *state = (RECIPE_STATE *)recipe->state;


   // process each task in sequence
   for (TASK *task = recipe->tasks; task != NULL; task = task->next) {
       if (execute_task(task) != 0) {
           // task failed. set the recipe's failed status
           state->failed = 1;
           return;
       }
   }
   // all tasks completed successfully
   state->failed = 0;
   state->completed = 1;
}


int execute_task(TASK *task)
{
   STEP *step;
   int num_steps = 0;
   int i = 0;
   int status = 0;
   int task_failed = 0;
   pid_t *child_pids = NULL;
   int **pipes = NULL;
   int input_fd = -1;
   int output_fd = -1;


   // count the number of steps in the task
   for (step = task->steps; step != NULL; step = step->next)
   {
       num_steps++;
   }


   if (num_steps == 0)
   {
       // no steps to execute
       return 0;
   }


   // allocate arrays for child PIDs & pipes
   child_pids = malloc(num_steps * sizeof(pid_t));
   if (child_pids == NULL)
   {
       perror("malloc");
       return -1;
   }


   if (num_steps > 1)
   {
       pipes = malloc((num_steps - 1) * sizeof(int *));
       if (pipes == NULL)
       {
           perror("malloc");
           free(child_pids);
           return -1;
       }
       for (i = 0; i < num_steps - 1; i++)
       {
           pipes[i] = malloc(2 * sizeof(int));
           if (pipes[i] == NULL)
           {
               perror("malloc");
               // free previously allocated pipes
               for (int j = 0; j < i; j++)
               {
                   free(pipes[j]);
               }
               free(pipes);
               free(child_pids);
               return -1;
           }
           // create the pipe
           if (pipe(pipes[i]) == -1)
           {
               perror("pipe");
               // free allocated resources
               for (int j = 0; j <= i; j++)
               {
                   free(pipes[j]);
               }
               free(pipes);
               free(child_pids);
               return -1;
           }
       }
   }


   // open input file if specified
   if (task->input_file != NULL)
   {
       input_fd = open(task->input_file, O_RDONLY);
       if (input_fd == -1)
       {
           fprintf(stderr, "Error: Cannot open input file '%s': %s\n", task->input_file, strerror(errno));
           // clean up resources
           if (pipes != NULL)
           {
               for (i = 0; i < num_steps - 1; i++)
               {
                   close(pipes[i][0]);
                   close(pipes[i][1]);
                   free(pipes[i]);
               }
               free(pipes);
           }
           free(child_pids);
           return -1;
       }
   }


   // open output file if specified
   if (task->output_file != NULL)
   {
       output_fd = open(task->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
       if (output_fd == -1)
       {
           fprintf(stderr, "Error: Cannot open output file '%s': %s\n", task->output_file, strerror(errno));
           // clean up resources
           if (input_fd != -1)
               close(input_fd);
           if (pipes != NULL)
           {
               for (i = 0; i < num_steps - 1; i++)
               {
                   close(pipes[i][0]);
                   close(pipes[i][1]);
                   free(pipes[i]);
               }
               free(pipes);
           }
           free(child_pids);
           return -1;
       }
   }


   // reset step pointer
   step = task->steps;
   for (i = 0; i < num_steps; i++)
   {
       pid_t pid = fork();
       if (pid == -1)
       {
           perror("fork");
           task_failed = 1;
           break;
       }
       else if (pid == 0)
       {
           // child process
           // set up input redirection
           if (i == 0)
           {
               // first step
               if (input_fd != -1)
               {
                   if (dup2(input_fd, STDIN_FILENO) == -1)
                   {
                       perror("dup2");
                       exit(EXIT_FAILURE);
                   }
               }
           }
           else
           {
               // not the first step, get input from previous pipe
               if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1)
               {
                   perror("dup2");
                   exit(EXIT_FAILURE);
               }
           }


           // set up output redirection
           if (i == num_steps - 1)
           {
               // last step
               if (output_fd != -1)
               {
                   if (dup2(output_fd, STDOUT_FILENO) == -1)
                   {
                       perror("dup2");
                       exit(EXIT_FAILURE);
                   }
               }
           }
           else
           {
               // not the last step, output to next pipe
               if (dup2(pipes[i][1], STDOUT_FILENO) == -1)
               {
                   perror("dup2");
                   exit(EXIT_FAILURE);
               }
           }


           // close unused file descriptors
           if (input_fd != -1)
               close(input_fd);
           if (output_fd != -1)
               close(output_fd);
           if (pipes != NULL)
           {
               for (int j = 0; j < num_steps - 1; j++)
               {
                   close(pipes[j][0]);
                   close(pipes[j][1]);
               }
           }


           // try to execute the command
           char *command = step->words[0];
           char util_command_path[1024];
           snprintf(util_command_path, sizeof(util_command_path), "util/%s", command);


           // check if the command exists in util
           if (access(util_command_path, X_OK) == 0)
           {
               // command exists in util. execute it
               execv(util_command_path, step->words);
               // if execv fails
               fprintf(stderr, "Error: Failed to execute '%s': %s\n", util_command_path, strerror(errno));
               exit(EXIT_FAILURE);
           }
           else
           {
               // command not in util. try execvp
               execvp(command, step->words);
               // if execvp fails
               fprintf(stderr, "Error: Failed to execute '%s': %s\n", command, strerror(errno));
               exit(EXIT_FAILURE);
           }
       }
       else
       {
           // parent process
           child_pids[i] = pid;


           // close unused file descriptors
           if (i > 0)
           {
               // close read end of previous pipe
               close(pipes[i - 1][0]);
           }
           if (i < num_steps - 1)
           {
               // close write end of current pipe
               close(pipes[i][1]);
           }
       }
       // move to the next step
       step = step->next;
   }


   // close remaining file descriptors in the parent
   if (input_fd != -1)
       close(input_fd);
   if (output_fd != -1)
       close(output_fd);
   if (pipes != NULL)
   {
       for (i = 0; i < num_steps - 1; i++)
       {
           // close remaining pipe ends
           close(pipes[i][0]);
           close(pipes[i][1]);
           free(pipes[i]);
       }
       free(pipes);
   }


   if (task_failed)
   {
       // if fork failed, wait for already forked children
       for (int j = 0; j < i; j++)
       {
           waitpid(child_pids[j], NULL, 0);
       }
       free(child_pids);
       return -1;
   }


   // wait for all child processes
   int task_exit_status = 0;
   for (i = 0; i < num_steps; i++)
   {
       pid_t wpid = waitpid(child_pids[i], &status, 0);
       if (wpid == -1)
       {
           perror("waitpid");
           task_failed = 1;
       }
       else
       {
           if (WIFEXITED(status))
           {
               int exit_status = WEXITSTATUS(status);
               if (exit_status != 0)
               {
                   task_failed = 1;
                   task_exit_status = exit_status;
               }
           }
           else if (WIFSIGNALED(status))
           {
               task_failed = 1;
           }
       }
   }


   free(child_pids);


   if (task_failed)
   {
       return task_exit_status ? task_exit_status : -1;
   }


   return 0; // task completed successfully
}


void cleanup(COOKBOOK *cbp)
{
   // free the state associated with each recipe
   for (RECIPE *rp = cbp->recipes; rp != NULL; rp = rp->next)
   {
       if (rp->state != NULL)
       {
           free(rp->state);
           rp->state = NULL;
       }
   }
}

