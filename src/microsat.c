#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum EXIT_CODES { OK = 0, ERROR = 1, SAT = 10, UNSAT = 20, BUILDABLE = 30, INCOMPLETE = 40, INVALID = 50 };
enum LITERAL_MARKS { END = -9, MARK = 2, IMPLIED = 6 };
enum MODES { MODE_SOLVE = 0, MODE_PROPAGATE = 1, MODE_STATUS = 2 };

const int MEM_MAX = 1 << 28;
int MODE = MODE_SOLVE;

struct solver { // The variables in the struct are described in the allocate procedure
  int  *DB, nVars, nClauses, mem_used, mem_fixed, maxLemmas, nLemmas, *buffer, nConflicts, *model,
       *reason, *falseStack, *false, *first, *forced, *processed, *assigned, *next, *prev, head, res, fast, slow,
       nDeadVars, *deadVars, nAssignments, *assignments; };

void unassign (struct solver* S, int lit) { S->false[lit] = 0; }   // Unassign the literal

void restart (struct solver* S) {                                  // Perform a restart (i.e., unassign all variables)
  while (S->assigned > S->forced) unassign (S, *(--S->assigned));  // Remove all unforced false lits from falseStack
  S->processed = S->forced; }                                      // Reset the processed pointer

void assign (struct solver* S, int* reason, int forced) {          // Make the first literal of the reason true
  int lit = reason[0];                                             // Let lit be the first literal in the reason
  S->false[-lit] = forced ? IMPLIED : 1;                           // Mark lit as true and IMPLIED if forced
  *(S->assigned++) = -lit;                                         // Push it on the assignment stack
  S->reason[abs (lit)] = 1 + (int) ((reason)-S->DB);               // Set the reason clause of lit
  S->model [abs (lit)] = (lit > 0); }                              // Mark the literal as true in the model

void addWatch (struct solver* S, int lit, int mem) {               // Add a watch pointer to a clause containing lit
  S->DB[mem] = S->first[lit]; S->first[lit] = mem; }               // By updating the database and the pointers

int* getMemory (struct solver* S, int mem_size) {                  // Allocate memory of size mem_size
  if (S->mem_used + mem_size > MEM_MAX) {                          // In case the code is used within a code base
    printf ("c OUT OF MEMORY\n"); exit (ERROR); }
  int *store = (S->DB + S->mem_used);                              // Compute a pointer to the new memory location
  S->mem_used += mem_size;                                         // Update the size of the used memory
  return store; }                                                  // Return the pointer

int* addClause (struct solver* S, int* in, int size, int irr) {    // Adds a clause stored in *in of size size
  int i, used = S->mem_used;                                       // Store a pointer to the beginning of the clause
  int* clause = getMemory (S, size + 3) + 2;                       // Allocate memory for the clause in the database
  if (size >  1) { addWatch (S, in[0], used  );                    // If the clause is not unit, then add
                   addWatch (S, in[1], used+1); }                  // Two watch pointers to the datastructure
  for (i = 0; i < size; i++) clause[i] = in[i]; clause[i] = 0;     // Copy the clause from the buffer to the database
  if (irr) S->mem_fixed = S->mem_used; else S->nLemmas++;          // Update the statistics
  return clause; }                                                 // Return the pointer to the clause is the database

void reduceDB (struct solver* S, int k) {                     // Removes "less useful" lemmas from DB
  while (S->nLemmas > S->maxLemmas) S->maxLemmas += 300;      // Allow more lemmas in the future
  S->nLemmas = 0;                                             // Reset the number of lemmas

  int i; for (i = -S->nVars; i <= S->nVars; i++) {            // Loop over the variables
    if (i == 0) continue; int* watch = &S->first[i];          // Get the pointer to the first watched clause
    while (*watch != END)                                     // As long as there are watched clauses
      if (*watch < S->mem_fixed) watch = (S->DB + *watch);    // Remove the watch if it points to a lemma
      else                      *watch =  S->DB[  *watch]; }  // Otherwise (meaning an input clause) go to next watch

  int old_used = S->mem_used; S->mem_used = S->mem_fixed;     // Virtually remove all lemmas
  for (i = S->mem_fixed + 2; i < old_used; i += 3) {          // While the old memory contains lemmas
    int count = 0, head = i;                                  // Get the lemma to which the head is pointing
    while (S->DB[i]) { int lit = S->DB[i++];                  // Count the number of literals
      if ((lit > 0) == S->model[abs (lit)]) count++; }        // That are satisfied by the current model
    if (count < k) addClause (S, S->DB+head, i-head, 0); } }  // If the latter is smaller than k, add it back

void bump (struct solver* S, int lit) {                       // Move the variable to the front of the decision list
  if (S->false[lit] != IMPLIED) { S->false[lit] = MARK;       // MARK the literal as involved if not a top-level unit
    int var = abs (lit); if (var != S->head) {                // In case var is not already the head of the list
      S->prev[S->next[var]] = S->prev[var];                   // Update the prev link, and
      S->next[S->prev[var]] = S->next[var];                   // Update the next link, and
      S->next[S->head] = var;                                 // Add a next link to the head, and
      S->prev[var] = S->head; S->head = var; } } }            // Make var the new head

int implied (struct solver* S, int lit) {                  // Check if lit(eral) is implied by MARK literals
  if (S->false[lit] > MARK) return (S->false[lit] & MARK); // If checked before return old result
  if (!S->reason[abs (lit)]) return 0;                     // In case lit is a decision, it is not implied
  int* p = (S->DB + S->reason[abs (lit)] - 1);             // Get the reason of lit(eral)
  while (*(++p))                                           // While there are literals in the reason
    if ((S->false[*p] ^ MARK) && !implied (S, *p)) {       // Recursively check if non-MARK literals are implied
      S->false[lit] = IMPLIED - 1; return 0; }             // Mark and return not implied (denoted by IMPLIED - 1)
  S->false[lit] = IMPLIED; return 1; }                     // Mark and return that the literal is implied

int* analyze (struct solver* S, int* clause) {         // Compute a resolvent from falsified clause
  S->res++; S->nConflicts++;                           // Bump restarts and update the statistic
  while (*clause) bump (S, *(clause++));               // MARK all literals in the falsified clause
  while (S->reason[abs (*(--S->assigned))]) {          // Loop on variables on falseStack until the last decision
    if (S->false[*S->assigned] == MARK) {              // If the tail of the stack is MARK
      int *check = S->assigned;                        // Pointer to check if first-UIP is reached
      while (S->false[*(--check)] != MARK)             // Check for a MARK literal before decision
        if (!S->reason[abs(*check)]) goto build;       // Otherwise it is the first-UIP so break
      clause = S->DB + S->reason[abs (*S->assigned)];  // Get the reason and ignore first literal
      while (*clause) bump (S, *(clause++)); }         // MARK all literals in reason
    unassign (S, *S->assigned); }                      // Unassign the tail of the stack

  build:; int size = 0, lbd = 0, flag = 0;             // Build conflict clause; Empty the clause buffer
  int* p = S->processed = S->assigned;                 // Loop from tail to front
  while (p >= S->forced) {                             // Only literals on the stack can be MARKed
    if ((S->false[*p] == MARK) && !implied (S, *p)) {  // If MARKed and not implied
      S->buffer[size++] = *p; flag = 1; }              // Add literal to conflict clause buffer
    if (!S->reason[abs (*p)]) { lbd += flag; flag = 0; // Increase LBD for a decision with a true flag
      if (size == 1) S->processed = p; }               // And update the processed pointer
    S->false[*(p--)] = 1; }                            // Reset the MARK flag for all variables on the stack

  S->fast -= S->fast >>  5; S->fast += lbd << 15;      // Update the fast moving average
  S->slow -= S->slow >> 15; S->slow += lbd <<  5;      // Update the slow moving average

  while (S->assigned > S->processed)                   // Loop over all unprocessed literals
    unassign (S, *(S->assigned--));                    // Unassign all lits between tail & head
  unassign (S, *S->assigned);                          // Assigned now equal to processed
  S->buffer[size] = 0;                                 // Terminate the buffer (and potentially print clause)
  return addClause (S, S->buffer, size, 0); }          // Add new conflict clause to redundant DB

int propagate (struct solver* S) {                  // Performs unit propagation
  int forced = S->reason[abs (*S->processed)];      // Initialize forced flag
  while (S->processed < S->assigned) {              // While unprocessed false literals
    int lit = *(S->processed++);                    // Get first unprocessed literal
    int* watch = &S->first[lit];                    // Obtain the first watch pointer
    while (*watch != END) {                         // While there are watched clauses (watched by lit)
      int i, unit = 1;                              // Let's assume that the clause is unit
      int* clause = (S->DB + *watch + 1);           // Get the clause from DB
      if (clause[-2] ==   0) clause++;              // Set the pointer to the first literal in the clause
      if (clause[ 0] == lit) clause[0] = clause[1]; // Ensure that the other watched literal is in front
      for (i = 2; unit && clause[i]; i++)           // Scan the non-watched literals
        if (!S->false[clause[i]]) {                 // When clause[i] is not false, it is either true or unset
          clause[1] = clause[i]; clause[i] = lit;   // Swap literals
          int store = *watch; unit = 0;             // Store the old watch
          *watch = S->DB[*watch];                   // Remove the watch from the list of lit
          addWatch (S, clause[1], store); }         // Add the watch to the list of clause[1]
      if (unit) {                                   // If the clause is indeed unit
        clause[1] = lit; watch = (S->DB + *watch);  // Place lit at clause[1] and update next watch
        if ( S->false[-clause[0]]) continue;        // If the other watched literal is satisfied continue
        if (!S->false[ clause[0]]) {                // If the other watched literal is falsified,
          assign (S, clause, forced); }             // A unit clause is found, and the reason is set
        else { if (forced) return UNSAT;            // Found a root level conflict -> UNSAT
          int* lemma = analyze (S, clause);         // Analyze the conflict return a conflict clause
          if (!lemma[1]) forced = 1;                // In case a unit clause is found, set forced flag
          assign (S, lemma, forced); break; } } } } // Assign the conflict clause as a unit
  if (forced) S->forced = S->processed;             // Set S->forced if applicable
  return SAT; }                                     // Finally, no conflict was found

int allVariablesAssigned (struct solver* S) {
  int nVarsAssigned = 0;
  for (int i = -S->nVars; i <= S->nVars; i++) {
    if (S->false[i]) nVarsAssigned++; }
  return nVarsAssigned == S->nVars; }

int evaluateClauses (struct solver* S) {
  int clauseStatus = 1;
  while (S->processed < S->assigned) {
    int lit = *(S->processed++);
    int* watch = &S->first[lit];
    while (*watch != END) {
      int i, unit = 1;
      int *clause = (S->DB + *watch + 1);
      if (clause[-2] == 0) clause++;
      if (clause[0] == lit) clause[0] = clause[1];
      for (i = 2; unit && clause[i]; i++)
        if (!S->false[clause[i]]) {
          clause[1] = clause[i]; clause[i] = lit;
          int store = *watch; unit = 0;
          *watch = S->DB[*watch];
          addWatch (S, clause[1], store); }
      if (unit) {
        clause[1] = lit; watch = (S->DB + *watch);
        if (S->false[-clause[0]] || !S->false[ clause[0]]) continue;
        else {
          clauseStatus = 0;
          return clauseStatus; } } } }
  return clauseStatus; }

int evaluateAssignment (struct solver* S) {
  for (int i = 0; i < S->nAssignments; i++) {
    if (S->false[S->assignments[i]]) {
      return 0; }
    for (int j = 0; j < S->nDeadVars; j++) {
      if (S->deadVars[j] == -S->assignments[i]) {
        return 0; } }
    assign (S, &S->assignments[i], 1);
    if (!evaluateClauses (S)) {
      return 0; } }
  return 1; }

int evaluateBuildability (struct solver* S) {
  if (!allVariablesAssigned (S)) {
    for (int i = 1; i <= S->nVars; i++) {
      if (!S->model[i] && !S->false[i]) {
        int lemma = -i;
        assign (S, &lemma, 0);
        if (!evaluateClauses (S)) {
            return 0; } } } }
  return 1; }

void evaluateDecisions (struct solver* S) {
  for (int i = 0; i < S->nDeadVars; i++) {
    assign (S, &S->deadVars[i], 1); }
  propagate (S);

  for (int i = S->nAssignments-1; i >= 0; i--) {
    int* lemma = &S->assignments[i];
    if ((S->assignments[i] > 0 && !S->model[S->assignments[i]]) || (S->assignments[i] < 0 && !S->false[S->assignments[i]])) {
      assign (S, lemma, 0);
      propagate (S); } } }

void printDecisions (struct solver* S) {
  printf ("v");
  for (int i = 1; i <= S->nVars; i++) {
    if (S->model[i] && (S->false[-i] == IMPLIED)) {
      printf (" %i", i); }
      else if (S->false[i] == IMPLIED) {
      printf (" %i", -i); } }
  printf ("\n"); }

int solve (struct solver* S) {                                      // Determine satisfiability
  int decision = S->head;                                           // Initialize the solver
  for (;;) {                                                        // Main solve loop
    int old_nLemmas = S->nLemmas;                                   // Store nLemmas to see whether propagate adds lemmas
    if (propagate (S) == UNSAT) return UNSAT;                       // Propagation returns UNSAT for a root level conflict

    if (S->nLemmas > old_nLemmas) {                                 // If the last decision caused a conflict
      decision = S->head;                                           // Reset the decision heuristic to head
      if (S->fast > (S->slow / 100) * 125) {                        // If fast average is substantially larger than slow average
        S->res = 0; S->fast = (S->slow / 100) * 125; restart (S);   // Restart and update the averages
        if (S->nLemmas > S->maxLemmas) reduceDB (S, 6); } }         // Reduce the DB when it contains too many lemmas

    while (S->false[decision] || S->false[-decision]) {             // As long as the temporary decision is assigned
      decision = S->prev[decision]; }                               // Replace it with the next variable in the decision list
    if (decision == 0) return SAT;                                  // If the end of the list is reached, then a solution is found
    decision = S->model[decision] ? decision : -decision;           // Otherwise, assign the decision variable based on the model
    S->false[-decision] = 1;                                        // Assign the decision literal to true (change to IMPLIED-1?)
    *(S->assigned++) = -decision;                                   // And push it on the assigned stack
    decision = abs(decision); S->reason[decision] = 0; } }          // Decisions have no reason clauses

void initDatabase (struct solver* S) {
  S->mem_used       = 0;                              // The number of integers allocated in the DB
  S->DB = (int *) malloc (sizeof (int) * MEM_MAX);    // Allocate the initial database
  S->DB[S->mem_used++] = 0; }                         // Make sure there is a 0 before the clauses are loaded

void initCDCL (struct solver* S, int n, int m) {
  if (n < 1)      n = 1;                  // The code assumes that there is at least one variable
  S->nVars          = n;                  // Set the number of variables
  S->nClauses       = m;                  // Set the number of clauses
  S->nLemmas        = 0;                  // The number of learned clauses -- redundant means learned
  S->nConflicts     = 0;                  // Under of conflicts which is used to updates scores
  S->maxLemmas      = 2000;               // Initial maximum number of learnt clauses
  S->res = 0;                             // Initialize restarts
  S->fast = S->slow = 1 << 24;            // Initialize the fast and slow moving averages

  S->model       = getMemory (S, n+1); // Full assignment of the (Boolean) variables (initially set to false)
  S->next        = getMemory (S, n+1); // Next variable in the heuristic order
  S->prev        = getMemory (S, n+1); // Previous variable in the heuristic order
  S->buffer      = getMemory (S, n  ); // A buffer to store a temporary clause
  S->reason      = getMemory (S, n+1); // Array of clauses
  S->falseStack  = getMemory (S, n+1); // Stack of falsified literals -- this pointer is never changed
  S->forced      = S->falseStack;      // Points inside *falseStack at first decision (unforced literal)
  S->processed   = S->falseStack;      // Points inside *falseStack at first unprocessed literal
  S->assigned    = S->falseStack;      // Points inside *falseStack at last unprocessed literal
  S->false       = getMemory (S, 2*n+1); S->false += n; // Labels for variables, non-zero means false
  S->first       = getMemory (S, 2*n+1); S->first += n; // Offset of the first watched clause

  int i; for (i = 1; i <= n; i++) {                        // Initialize the main data structures:
    S->prev [i] = i - 1; S->next[i-1] = i;                 // the double-linked list for variable-move-to-front,
    S->model[i] = S->false[-i] = S->false[i] = 0;          // the model (phase-saving), the false array,
    S->first[i] = S->first[-i] = END; }                    // and first (watch pointers).
  S->head = n; }                                           // Initialize the head of the double-linked list

int parse (struct solver* S, char* filename) {                            // Parse the formula and initialize
  int tmp; FILE* input = fopen (filename, "r");                           // Read the CNF file
  if (input == NULL) printf ("c FILE NOT FOUND\n"), exit (ERROR);         // Exit if file not found

  initDatabase(S);

  if (MODE == MODE_PROPAGATE || MODE == MODE_STATUS) {                    // Parse the additional comment lines
    int i;

    do { tmp = fscanf (input, " c d%i", &S->nDeadVars);                   // Parse "dead" (i.e. always false) variables
      if (tmp > 0 && tmp != EOF) break; tmp = fscanf (input, "%*s\n"); }  // Skip rest of line
    while (tmp != 1 && tmp != EOF);
    S->deadVars = getMemory (S, S->nDeadVars);
    i = 0;
    while (i < S->nDeadVars) {
      fscanf (input, "%i", &tmp);
      S->deadVars[i++] = -tmp; }
    fseek (input, 0, SEEK_SET);                                           // Reset file position

    do { tmp = fscanf (input, " c v%i", &S->nAssignments);                // Parse assigned variables
      if (tmp > 0 && tmp != EOF) break; tmp = fscanf (input, "%*s\n"); }  // Skip rest of line
    while (tmp != 1 && tmp != EOF);
    S->assignments = getMemory (S, S->nAssignments);
    i = 0;
    while (i < S->nAssignments) {
      fscanf (input, "%i", &S->assignments[i++]); }
    fseek (input, 0, SEEK_SET);                                           // Reset file position
  }

  do { tmp = fscanf (input, " p cnf %i %i \n", &S->nVars, &S->nClauses);  // Find the first non-comment line
    if (tmp > 0 && tmp != EOF) break; tmp = fscanf (input, "%*s\n"); }    // In case a comment line was found
  while (tmp != 2 && tmp != EOF);                                         // Skip it and read next line

  initCDCL (S, S->nVars, S->nClauses);                     // Allocate the main datastructures
  int nZeros = S->nClauses, size = 0;                      // Initialize the number of clauses to read
  while (nZeros > 0) {                                     // While there are clauses in the file
    int lit = 0; tmp = fscanf (input, " %i ", &lit);       // Read a literal.
    if (!lit) {                                            // If reaching the end of the clause
      int* clause = addClause (S, S->buffer, size, 1);     // Then add the clause to data_base
      if (!size || ((size == 1) && S->false[clause[0]]))   // Check for empty clause or conflicting unit
        return UNSAT;                                      // If either is found return UNSAT
      if ((size == 1) && !S->false[-clause[0]]) {          // Check for a new unit
        assign (S, clause, 1); }                           // Directly assign new units (forced = 1)
      size = 0; --nZeros; }                                // Reset buffer
    else S->buffer[size++] = lit; }                        // Add literal to buffer
  fclose (input);                                          // Close the formula file
  return SAT; }                                            // Return that no conflict was observed

int main (int argc, char** argv) {                                                                          // The main procedure
  if (argc == 1) printf ("Usage: microsat [--version] [--status | --propagate] DIMACS_FILE\n"), exit (OK);  // Print usage if no argument is given
  if (!strcmp (argv[1], "--version")) printf (VERSION "\n"), exit (OK);                                     // Print version if argument --version is given
  else if (!strcmp (argv[1], "--status")) MODE = MODE_STATUS, ++argv;                                       // Set mode to check status of an assignment
  else if (!strcmp (argv[1], "--propagate")) MODE = MODE_PROPAGATE, ++argv;                                 // Set mode to propagate an assignment

  struct solver S;                                                                        // Create the solver datastructure
  if (parse (&S, argv[1]) == UNSAT) printf("s UNSATISFIABLE\n"), exit (UNSAT);            // Parse the DIMACS file

  if (MODE == MODE_PROPAGATE) {
    evaluateDecisions (&S);
    printDecisions (&S);
    if (evaluateBuildability(&S)) printf ("s BUILDABLE\n"), exit (BUILDABLE);
    else printf ("s INCOMPLETE\n"), exit (INCOMPLETE); }
  else if (MODE == MODE_STATUS) {
    if (evaluateAssignment (&S)) {
      if (evaluateBuildability (&S)) printf ("s BUILDABLE\n"), exit (BUILDABLE);
      else printf ("s INCOMPLETE\n"), exit (INCOMPLETE);
    }
    else printf ("s INVALID\n"), exit (INVALID); }
  else if (MODE == MODE_SOLVE) {
    if (solve (&S) == UNSAT) printf("s UNSATISFIABLE\n"), exit (UNSAT);                   // Solve without limit (number of conflicts)
    else printf("s SATISFIABLE\n"), exit (SAT); } }                                       // and print whether the formula has a solution