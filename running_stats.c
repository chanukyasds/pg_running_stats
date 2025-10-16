

/* running_stats.c
 * Mergeable running statistics for PostgreSQL using Welford/Chan.
 *
 * Exposes:
 *   - rstat_sfunc(bytea, float8) -> bytea               (SFUNC)
 *   - rstat_combine(bytea, bytea) -> bytea              (COMBINEFUNC)
 *   - rstat_final(bytea) -> rstat_result_t              (FINALFUNC / decoder)
 *   - Aggregates:
 *       * running_stats(float8) -> rstat_result_t       (one-run, mergeable)
 *       * rstat_state(float8) -> bytea                  (raw state, mergeable)
 *   - Helpers:
 *       * rstat_state_merge(bytea, bytea) -> bytea      (alias to combine)
 *       * rstat_state_result(bytea) -> rstat_result_t   (alias to final)
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include <math.h>

PG_MODULE_MAGIC;

/*
 * rs_state
 * ----------
 * Transition state for running statistics using Welford’s method
 * with higher moments (West 1979; Pébay 2008).
 *
 * Fields:
 *   n      : number of observations seen
 *   mean   : running mean
 *   M2,M3,M4 : running centered sums for variance, skewness, kurtosis
 *   minv,maxv : running min/max
 *
 * Notes:
 * - Memory footprint is fixed (56 bytes), suitable for aggregate STYPE.
 * - Supports numerically stable one-pass updates and later merging.
 */
typedef struct
{
    int64  n;
    double mean, M2, M3, M4;
    double minv, maxv;
} rs_state;

/*
 * rs_init
 * ----------
 * Initialize a fresh rs_state.
 *
 * Contract:
 * - Sets count and all moments to zero.
 * - Initializes min/max to +/-INF so first sample snaps them to real values.
 * Complexity: O(1)
 */
static inline void rs_init(rs_state *s)
{
    s->n = 0;
    s->mean = s->M2 = s->M3 = s->M4 = 0.0;
    s->minv = +INFINITY;
    s->maxv = -INFINITY;
}

/*
 * rs_add
 * ----------
 * Update the running state with a single new observation x.
 *
 * Method:
 * - Welford/West update for mean and centered moments (M2..M4),
 *   numerically stable in one pass.
 * - Also updates min/max.
 *
 * Inputs:
 *   s : pointer to state (modified in place)
 *   x : new sample
 *
 * Output:
 *   s is updated; no return value.
 * Complexity: O(1) per sample.
 */
static inline void rs_add(rs_state *s, double x)
{
    int64  n1;
    double n;
    double delta, delta_n, delta_n2, term1;

    n1 = s->n;          
    s->n++;             
    n  = (double)s->n;

    delta    = x - s->mean;
    delta_n  = delta / n;
    delta_n2 = delta_n * delta_n;
    term1    = delta * delta_n * n1;

    s->mean += delta_n;
    s->M4   += term1*delta_n2*(n*n - 3*n + 3) + 6*delta_n2*s->M2 - 4*delta_n*s->M3;
    s->M3   += term1*delta_n*(n - 2) - 3*delta_n*s->M2;
    s->M2   += term1;

    if (x < s->minv) s->minv = x;
    if (x > s->maxv) s->maxv = x;
}

/*
 * rs_merge
 * ----------
 * Merge state B into state A using Chan/Pébay parallel-combination
 * formulas for the first four centered moments.
 *
 * Purpose:
 * - Enables parallel aggregation and post-hoc composition of partitions:
 *   combine(A_of_left, A_of_right) == A_of_all.
 *
 * Behavior:
 * - Preserves min/max across states.
 * - Updates A in place to represent A ∪ B.
 *
 * Preconditions:
 * - States must share the same layout (sizeof(rs_state)).
 * - Intended for valid states produced by rs_add/other merges.
 *
 * Notes:
 * - Uses numerically stable formulas; O(1) time, O(1) space.
 */
static inline void rs_merge(rs_state *A, const rs_state *B)
{
    
    double n1 = (double)A->n ;
    double n2 = (double)B->n ;
    double n = n1 + n2;
    double delta  = B->mean - A->mean;
    double delta2 = delta*delta, delta3 = delta2*delta, delta4 = delta2*delta2;

    double M2 = A->M2 + B->M2 + delta2 * (n1*n2) / n;
    double M3 = A->M3 + B->M3
              + delta3 * (n1*n2*(n1 - n2) / (n*n))
              + 3.0 * delta * (n1*B->M2 - n2*A->M2) / n;
    double M4 = A->M4 + B->M4
              + delta4 * (n1*n2*(n1*n1 - n1*n2 + n2*n2) / (n*n*n))
              + 6.0 * delta2 * ((n1*n1*B->M2 + n2*n2*A->M2) / (n*n))
              + 4.0 * delta * (n1*B->M3 - n2*A->M3) / n;

    
    if (B->n == 0) return;
    if (A->n == 0) { *A = *B; return; }

    if (B->minv < A->minv) A->minv = B->minv;
    if (B->maxv > A->maxv) A->maxv = B->maxv;

    A->mean = A->mean + delta * (n2 / n);
    A->M2 = M2; A->M3 = M3; A->M4 = M4;
    A->n += B->n;
}

/* ---------- SQL bindings ---------- */
PG_FUNCTION_INFO_V1(rstat_sfunc);
PG_FUNCTION_INFO_V1(rstat_combine);
PG_FUNCTION_INFO_V1(rstat_final);

/*
 * rstat_sfunc
 * -------------
 * Aggregate transition function: (bytea state, float8 x) -> bytea state
 *
 * Responsibilities:
 * - Initialize a fresh rs_state when state is NULL.
 * - Detoast/read existing state and update it via rs_add(x).
 * - Return the (possibly newly allocated) bytea containing the state.
 * - Checks the single state size to protect from malformed saved states.
 *
 * Contract:
 * - STYPE must be bytea of size sizeof(rs_state) + VARHDRSZ.
 * - Numerically stable one-pass update per row.
 * Parallel-safety: SAFE (no external I/O).
 * Complexity: O(1) per input row.
 */
Datum rstat_sfunc(PG_FUNCTION_ARGS)
{
    bytea   *state;
    rs_state s;
    double   x;         

    if (PG_ARGISNULL(0))
    {
        state = (bytea *) palloc0(VARHDRSZ + sizeof(rs_state));
        SET_VARSIZE(state, VARHDRSZ + sizeof(rs_state));
        rs_init(&s);
    }
    else
    {
        state = PG_GETARG_BYTEA_P(0);

        if (VARSIZE_ANY_EXHDR(state) != sizeof(rs_state))
            ereport(ERROR,(errmsg("invalid running-stats state size")));

        memcpy(&s, VARDATA_ANY(state), sizeof(s));
    }

    if (!PG_ARGISNULL(1))
    {
        x = PG_GETARG_FLOAT8(1);
        rs_add(&s, x);
        memcpy(VARDATA_ANY(state), &s, sizeof(s));
    }

    PG_RETURN_BYTEA_P(state);
}

/*
 * rstat_combine
 * ---------------
 * Aggregate combine function: (bytea a, bytea b) -> bytea
 *
 * Purpose:
 * - Merge two partial states (from parallel workers / group partitions).
 * - Writes result back into a writable copy of 'a' and returns it.
 *
 * Safety details:
 * - Uses PG_GETARG_BYTEA_P_COPY(0) to ensure 'a' is writable (not shared/toasted).
 * - Validates state sizes to guard against corrupt inputs.
 * - Frees any detoasted copy of 'b'.
 *
 * Parallel-safety: SAFE (pure compute on arguments).
 * Complexity: O(1)
 */
Datum rstat_combine(PG_FUNCTION_ARGS)
{
    bytea   *a;        /* will be a writable copy */
    bytea   *b;
    rs_state A, B;

    if (PG_ARGISNULL(0)) PG_RETURN_DATUM(PG_GETARG_DATUM(1));
    if (PG_ARGISNULL(1)) PG_RETURN_DATUM(PG_GETARG_DATUM(0));

    /* Make sure 'a' is a modifiable copy; 'b' can be a plain detoasted ptr */
    a = PG_GETARG_BYTEA_P_COPY(0);
    b = PG_GETARG_BYTEA_P(1);

    /* (Optional) guard against unexpected state sizes */
    if (VARSIZE_ANY_EXHDR(a) != sizeof(rs_state) || VARSIZE_ANY_EXHDR(b) != sizeof(rs_state))
        ereport(ERROR, (errmsg("invalid running-stats state size")));

    memcpy(&A, VARDATA_ANY(a), sizeof(A));
    memcpy(&B, VARDATA_ANY(b), sizeof(B));

    rs_merge(&A, &B);

    /* write back into 'a' (same size) */
    memcpy(VARDATA_ANY(a), &A, sizeof(A));

    /* free if detoasted copies were made */
    PG_FREE_IF_COPY(b, 1);  /* 'a' is already a copy we return */

    PG_RETURN_BYTEA_P(a);
}

/*
 * rstat_final
 * -------------
 * Aggregate final function: (bytea state) -> rstat_result_t
 *
 * Responsibilities:
 * - Interpret the transition state and produce human-friendly stats:
 *     n, mean, var (sample, n-1), stddev, skew, kurt, min, max
 * - Returns zeros when state is NULL (empty input).
 *
 * Numerical notes:
 * - Variance is sample variance (unbiased divisor n-1).
 * - Skew/kurtosis are computed only when moments are defined/stable:
 *   skew requires n>2 and M2>0; kurt requires n>3 and M2>0.
 *
 * Parallel-safety: SAFE (pure compute).
 * Complexity: O(1)
 */
Datum rstat_final(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum     values[8];
    bool      nulls[8] = {false,false,false,false,false,false,false,false};
    HeapTuple tup;      

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("rstat_final must be called in a record context")));
    BlessTupleDesc(tupdesc);

    if (PG_ARGISNULL(0))
    {
        int i;
        values[0] = Int64GetDatum(0);
        for (i = 1; i < 8; i++) values[i] = Float8GetDatum(0.0);
    }
    else
    {
        rs_state s;
        double var, sd, skew, kurt;

        memcpy(&s, VARDATA_ANY(PG_GETARG_BYTEA_P(0)), sizeof(s));

        var  = (s.n > 1) ? s.M2 / (double)(s.n - 1) : 0.0;  
        sd   = sqrt(var);
        skew = 0.0;
        kurt = 0.0;

        if (s.n > 2 && s.M2 > 0.0)
            skew = (sqrt((double)s.n) * s.M3) / pow(s.M2, 1.5);
        if (s.n > 3 && s.M2 > 0.0)
            kurt = ((double)s.n * s.M4) / (s.M2 * s.M2) - 3.0;

        values[0] = Int64GetDatum(s.n);
        values[1] = Float8GetDatum(s.mean);
        values[2] = Float8GetDatum(var);
        values[3] = Float8GetDatum(sd);
        values[4] = Float8GetDatum(skew);
        values[5] = Float8GetDatum(kurt);
        values[6] = Float8GetDatum(s.n ? s.minv : 0.0);
        values[7] = Float8GetDatum(s.n ? s.maxv : 0.0);
    }

    tup = heap_form_tuple(tupdesc, values, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tup));
}