
# pg_running_stats

Mergeable Running Statistics (Welford/Chan) for PostgreSQL

`pg_running_stats` provides numerically stable, single-pass, and mergeable running statistics directly inside PostgreSQL.
It computes mean, variance, standard deviation, skewness, kurtosis, and min/max efficiently in pure C.

---

## Features

- Single-pass computation using Welford’s algorithm  
- Mergeable states supporting parallel aggregation  
- Numerically stable for large and high-precision datasets  
- Zero dependencies (pure PostgreSQL and C99)  
- Cross-platform support for macOS (Homebrew) and Linux  

---

## Installation

### Prerequisites

PostgreSQL development headers must be installed locally.

### macOS (Homebrew)
```bash
brew install postgresql@17
make
make install
````

### Linux (Ubuntu/Debian)

```bash
sudo apt install postgresql-server-dev-17
make
sudo make install
```

Then inside psql:

```sql
CREATE EXTENSION pg_running_stats;
```

---

## Usage

### One-run aggregate

```sql
SELECT (running_stats(x)).*
FROM (VALUES (1.0),(2.0),(3.0),(4.0),(5.0)) t(x);
```

| n | mean | var | stddev | skew | kurt | min | max |
| - | ---- | --- | ------ | ---- | ---- | --- | --- |
| 5 | 3.0  | 2.5 | 1.5811 | 0.0  | -1.3 | 1.0 | 5.0 |

---

### Build and inspect a raw state

```sql
WITH s AS (
  SELECT rstat_state(x) AS st
  FROM (VALUES (1.0),(2.0),(3.0)) v(x)
)
SELECT (rstat_state_result(st)).* FROM s;
```

Produces identical statistics using a stored binary state (bytea).

---

### Merge states (parallel or incremental)

```sql
WITH a AS (SELECT rstat_state(x) st FROM (VALUES (1.0),(2.0)) t(x)),
     b AS (SELECT rstat_state(x) st FROM (VALUES (3.0),(4.0)) t(x))
SELECT (rstat_state_result(rstat_state_merge(a.st, b.st))).*
FROM a,b;
```

You can merge results from partitions, shards, or parallel workers without recomputation.

---

## SQL Interface Summary

| Function / Aggregate      | Input        | Output | Description                      |
| ------------------------- | ------------ | ------ | -------------------------------- |
| running_stats(x)          | float8       | record | Full statistics record           |
| rstat_state(x)            | float8       | bytea  | Raw internal state               |
| rstat_state_merge(a,b)    | bytea, bytea | bytea  | Merges two states                |
| rstat_state_result(state) | bytea        | record | Converts a state into statistics |

---

## Algorithm

`pg_running_stats` implements the Welford (1962) and Chan et al. (1979) algorithms for online variance and higher moments.

References:

* Welford, B.P. (1962). "Note on a method for calculating corrected sums of squares and products." *Technometrics*, 4(3):419–420.
* Chan, Tony F., Gene H. Golub, and Randall J. LeVeque. (1979). "Updating formulae and a pairwise algorithm for computing sample variances." *Technical Report STAN-CS-79-773*.

These algorithms ensure:

* Numerical stability for streaming and large datasets
* Mergeability for distributed and parallel aggregation
* Efficient double-precision computation (float8)

---

## Build Information

| Platform         | Status                        |
| ---------------- | ----------------------------- |
| macOS (Homebrew) | Tested on Ventura and Sequoia |
| Ubuntu / Debian  | Tested on 22.04 LTS and 24.04 |
| PostgreSQL       | Supported versions 13 – 17    |

---

## License

MIT License – see LICENSE file for details.

---

## Author

Chanukya Sista

---

## Contribute

If you find this useful:

* Star the repository
* Report issues or suggest improvements
* Fork and contribute enhancements


