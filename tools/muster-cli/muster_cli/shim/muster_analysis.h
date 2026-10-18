/* Force-included when muster-cli analyses a sketch (never when compiling for
 * a board). It turns the annotation into something libclang can see.      */
#define MUSTER_ANALYSIS 1
#define MUSTER_TASK(...) __attribute__((annotate("muster:" #__VA_ARGS__)))
