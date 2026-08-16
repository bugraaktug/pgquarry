# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

pgquarry — a Postgres-native async job queue for local LLM embedding inference (GGUF via `llama.cpp`, dequeued by an external worker process, no CDC, no external ML service). Split out of [walkrie](https://github.com/bugraaktug/walkrie)'s embedding-provider layer. Full scope, positioning, architecture, and roadmap (v0/v1/v1.x/v2) are in **README.md** — read it before starting any implementation work.

## Status

v0 shipped: the transplanted embedding path (mostly reused from walkrie, see README's reuse table) works standalone against a hand-populated `pgquarry.jobs` table. Now building v1 per README.md's Roadmap: `pgquarry.toml`-driven worker, a generic trigger (`pgquarry.enqueue_job()`) that enqueues on INSERT/UPDATE, write-back into the user's own table (same-table `UPDATE` or cross-table `UPSERT` via the `[[table]]` array), `NOTIFY` instead of polling, and two-stage job retention.

## Conventions (carried over from walkrie)

- **Sign off every commit** (`git commit -s`) — DCO requirement.
- License: Apache-2.0, matching walkrie — enables clean copy-with-attribution vendoring of walkrie's reused files.
- New config fields validated with specific, actionable error messages (see walkrie's `AppConfig::validate()` style) — not generic "invalid config".
- Prefer plain runtime `enum` + `switch`/if-else dispatch over templates for small, fixed-choice cases.
- Comments are short one-liners explaining *why*, not what — only when the reasoning isn't obvious from the code.
- One logical change per PR; explain *why* in the description for behavior changes.
