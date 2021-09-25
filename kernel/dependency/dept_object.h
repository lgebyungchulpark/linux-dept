/*
 * OBJECT(id, nr)
 *
 * id: Id for the object of struct dept_##id.
 * nr: # of the object that should be kept in the pool.
 */

OBJECT(dep	,1024 * 8)
OBJECT(class	,1024 * 4)
OBJECT(stack	,1024 * 16)
OBJECT(ecxt	,1024 * 4)
OBJECT(wait	,1024 * 8)
