# PES-VCS Report: Phase 5 and 6 (Analysis)

## Q5.1 Branching and Checkout
A branch is a ref file (for example, `.pes/refs/heads/feature`) containing a commit hash.

To implement `pes checkout <branch>`:
1. Resolve target commit hash by reading `.pes/refs/heads/<branch>`.
2. Update `.pes/HEAD` to `ref: refs/heads/<branch>` (unless supporting detached mode explicitly).
3. Read target commit object, then its root tree, then recursively materialize tree entries into working directory files.
4. Remove tracked files/directories from current branch that do not exist in target tree.
5. Update `.pes/index` to match target tree snapshot metadata/hash list.

Why complex:
1. Conflict handling with local modifications.
2. Safe file deletion/overwrite ordering.
3. Recursive tree expansion and path handling.
4. Atomicity: avoiding half-switched worktrees on failure.

## Q5.2 Dirty Working Directory Conflict Detection
Using only index + object store:
1. Load current index entries.
2. For each tracked path, stat working-directory file.
3. If file missing where index expects it, treat as local modification.
4. If mtime/size differ from index, recompute blob hash from file content and compare with index hash.
5. If recomputed hash != index hash, file is dirty.
6. Build set of paths that would change between current branch tree and target branch tree.
7. If any dirty path intersects changed-path set, refuse checkout and print conflicting paths.

This matches Git behavior: checkout is blocked only when local changes would be overwritten.

## Q5.3 Detached HEAD
Detached HEAD means `.pes/HEAD` stores a commit hash directly instead of `ref: ...`.

If commits are made in this state:
1. New commits are created with parent links normally.
2. No branch ref advances to those commits.
3. They can become unreachable later (and eventually garbage-collected).

Recovery:
1. Create a branch at current detached commit (for example, `refs/heads/recovered`) before leaving detached state.
2. Or use reflog-like history (if implemented) to find hash and then create a branch there.

## Q6.1 Garbage Collection Algorithm
Goal: delete unreachable objects.

Mark-and-sweep approach:
1. Initialize a hash set `reachable`.
2. Seed a stack/queue with all branch-tip commit hashes from `.pes/refs/heads/*` (and tags if present).
3. DFS/BFS over object graph:
   - Commit -> mark commit, then enqueue parent (if any) and root tree.
   - Tree -> mark tree, parse entries, enqueue child trees/blobs.
   - Blob -> mark blob only.
4. Sweep `.pes/objects/*/*`: for each object hash not in `reachable`, delete file.

Data structure:
1. Hash set for O(1) average membership checks.

Scale estimate for 100,000 commits and 50 branches:
1. Commits visited: up to about 100,000 unique (branch histories overlap heavily).
2. Trees/blobs: depends on project churn; commonly several multiples of commit count.
3. Total visited objects is often in the low millions for active repos.

## Q6.2 Why Concurrent GC Is Dangerous
Race example:
1. Commit process writes new tree/blob objects first.
2. Before branch ref is updated to point to new commit, those new objects are temporarily unreachable from refs.
3. Concurrent GC scans refs, does not see new commit path, and deletes those just-written objects.
4. Commit then updates ref to a commit that references missing objects -> repository corruption.

How real Git avoids this:
1. Uses reachability protections and grace periods.
2. Uses object mtimes and prune thresholds (`gc.pruneExpire`) to avoid deleting very recent loose objects.
3. Coordinates with lock files and transactional ref updates.
4. Uses packfile/reflog safety and conservative pruning policies.
