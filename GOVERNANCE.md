# Project Governance & Community Principles

Marginalia is a community-driven fork of CrossPoint Reader. Its goal is to keep a strong reader firmware while making
experimentation welcome through a package ecosystem.

The project is independent from CrossPoint Reader and Xteink. Upstream compatibility matters, but Marginalia's maintainers
may choose different tradeoffs when extension support, side loading, or ecosystem health requires it.

## 1. Human First

Technical discussions can get heated, but they should never be personal.

- **Assume good intent:** Most contributors are volunteers working in spare time and often across language barriers.
- **Focus on the work:** Critique the implementation, performance, UX, compatibility, or risk. Do not attack the person.
- **Keep the space usable:** Harassment, exclusionary language, repeated personal attacks, and bad-faith disruption are
  not welcome.

## 2. Reader First, Packages Welcome

Marginalia uses a two-layer contribution model.

- **Core firmware changes** should protect reading reliability, battery life, X3/X4 support, and maintainability.
- **Package ecosystem changes** should make it easier to build, validate, install, discover, and safely run optional
  packages.
- **New feature ideas** should usually start by asking whether the feature belongs in core firmware or in a package.
- **Hooks and APIs** are accepted when they serve real package use cases and can be kept stable.

For more guidance, see [SCOPE.md](SCOPE.md).

## 3. Open Workflows

We keep technical work in public whenever possible.

- Discuss ideas in [GitHub Discussions](https://github.com/marginalia-os/marginalia-firmware/discussions).
- Report firmware bugs in [GitHub Issues](https://github.com/marginalia-os/marginalia-firmware/issues).
- Use pull requests for code changes, docs changes, examples, and design proposals.
- Write issues and PRs with enough context that someone else can reproduce, review, or continue the work.

## 4. Fork Relationship

Marginalia is based on CrossPoint Reader and should stay close enough to upstream that useful reader improvements can be
merged forward. At the same time, Marginalia is allowed to carry extension-layer code that upstream does not want.

When changing inherited CrossPoint code:

- Prefer small, reviewable changes.
- Keep upstream compatibility in mind.
- Document deliberate divergence when it affects future merges.
- Avoid changing inherited behaviour unless it supports the reader or the package layer.

## 5. Moderation & Safety

Maintainers are responsible for keeping the community usable.

- Maintainers may hide comments, lock threads, close issues, or block users who repeatedly violate these principles.
- Security-sensitive reports should not be posted publicly until maintainers have had time to respond.
- If moderation or security contact details change, this file should be updated before the next release.
