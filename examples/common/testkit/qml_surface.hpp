// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QMap>
#include <QSet>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>

/// @file
/// The metaobject-surface drift guard, generalised out of the hand-written
/// one `examples/bookmarks/tests/test_bookmark_qml_bridges.cpp` established
/// (morph#86's "what is actually justified", third bullet).
///
/// @par What drifts, and why nothing else catches it
/// A rung's QML binds its bridges *by string*: `page.tagController.refresh()`
/// resolves through the metaobject at run time, and `function onListed(rows)`
/// inside a `Connections` block is matched against a signal name at run time
/// too. Nothing in the build sees either. The rungs' offscreen engine-load
/// smoke tests cannot see them either, and say so in their own file comments:
/// they load every QML root with every controller property left `null`, so no
/// `Connections` block ever has a live target and no handler name is ever
/// resolved against a real signal.
///
/// The hand-written guard the prior art carries closes half of that: it
/// enumerates the metaobject and `REQUIRE`s each name QML is *known* to bind,
/// then pins `methodCount() - methodOffset()` so a newly added member with no
/// binding site fails too. Both halves are a human transcription of the QML,
/// cited by file and line in a comment — which is exactly the artefact that
/// goes stale when the QML moves and the C++ does not.
///
/// @par What this does instead
/// `QmlSurfaceAudit` reads the rung's `.qml` files as the expectation. It
/// extracts every reference a bound alias receives — calls, property reads
/// and `Connections` signal handlers — and resolves each against the live
/// `QMetaObject`, then sweeps the other way and reports every own member of
/// the bridge that no `.qml` file references. There is no hand-written list
/// on either side, so neither side can drift without a finding.
///
/// @par The drift directions it covers
/// 1. **QML binds what the bridge does not have.** A handler `onListd`, a
///    call `refreshAll()` on a bridge that only has `refresh()`, a property
///    read of a `Q_PROPERTY` that was renamed. This is the direction the
///    hand-written guard structurally cannot cover, because its expectation
///    *is* the transcription being checked.
/// 2. **The bridge exposes what no QML binds.** A `Q_INVOKABLE` left behind
///    by a deleted screen, a signal nothing connects.
/// 3. **Arity disagreement.** `submitIfValid("Login")` against a two-argument
///    invokable; a handler declaring more parameters than the signal carries.
/// 4. **A bridge the audit was never handed.** A `Connections` block whose
///    `target` alias no `bind()` call named — the way a bridge silently ends
///    up with no guard at all.
/// 5. **An exemption that has outlived its reason.** See `allowUnbound()`.
///
/// @par What it does not cover — stated rather than implied
/// * **Types.** QML is dynamically typed at these call sites; the audit
///   matches on name and argument *count* only. `open(qlonglong)` called with
///   a string is a run-time coercion this cannot see.
/// * **Property-bag keys.** `row.modelData.url` reads a key of a
///   `QVariantMap` a bridge *emitted*; there is no metaobject for it. The
///   per-rung "bag shape" cases (which assert the exact key set of an emitted
///   bag) remain the only guard there, and are not replaced by this one.
/// * **QML the rung does not own.** A bridge handed to the shipped
///   `MorphForms` `DynamicForm` is referenced from *that* module's QML, not
///   the rung's. Add the renderer's directory with `addDirectory()`, or
///   record the member with `allowUnbound()` and a reason.
/// * **Dynamic member access.** `bridge[someName]()` and anything reached
///   through `Qt.createComponent` string sources are invisible to a scanner.
/// * **Whether the alias is wired to the class the audit was handed.** The
///   alias-to-instance mapping is the one genuinely per-rung fact, and it is
///   the test's `bind()` call, not something derived from the shell's
///   `setInitialProperties`. A shell that binds `tagController` to the wrong
///   object still passes.
///
/// @par Vacuity
/// A guard that measures nothing is the failure mode `AGENTS.md` names first,
/// so the audit reports one when it happens: no `.qml` files found, a missing
/// directory, a bound alias that no scanned file mentions, a bound bridge with
/// no own metaobject members at all, and `run()` with nothing bound are each a
/// finding in their own right.

namespace morph::ladder::testkit {

/// @brief How a `.qml` file referred to a bridge member.
enum class QmlReferenceKind : std::uint8_t {
    /// `alias.member(...)` — an invokable or a slot.
    Call,
    /// `alias.member` with no call parentheses — a `Q_PROPERTY` read.
    Read,
    /// `function onMember(...)` (or `onMember:`) inside a `Connections`
    /// block whose `target` is the alias — a signal, or a property's
    /// `NOTIFY` signal.
    SignalHandler,
};

/// @brief One reference to a bridge member found in a `.qml` file.
struct QmlReference {
    /// The member as the metaobject would name it: the identifier after the
    /// alias for a call or read, and the handler name minus its `on` prefix
    /// with the first letter lowered for a signal handler.
    QString member;
    /// The alias the reference was made through.
    QString alias;
    /// Which of the three reference shapes this is.
    QmlReferenceKind kind = QmlReferenceKind::Read;
    /// Arguments written at the site: the call's, or the handler's declared
    /// parameters. Zero for a read.
    int argumentCount = 0;
    /// File name (basename) the reference was found in.
    QString file;
    /// One-based line number of the reference.
    int line = 0;
};

/// @brief Everything one `.qml` file says about the bridges it binds.
///
/// Public because the audit's own self-test drives the scanner directly; a
/// rung's test has no reason to call it.
struct QmlScanResult {
    /// Every `alias.member` reference and every `Connections` handler,
    /// keyed by the alias the reference was made through.
    QMap<QString, QVector<QmlReference>> referencesByAlias;
    /// Aliases that are the `target` of a `Connections` block, in the
    /// `<id>.<alias>` shape every ladder rung writes. Used to notice a bridge
    /// the QML consumes signals from that the audit was never handed.
    QStringList connectionsTargets;
    /// `alias.member` for every member this file *probes* for existence, by
    /// comparing it against `undefined` or applying `typeof` to it.
    ///
    /// Such a read is a question, not a use: it is how QML asks whether a
    /// conditionally-compiled member is present in this build. kanban's
    /// `BoardView.qml` guards its dead-letter banner exactly this way, because
    /// `BoardBridge::deadLetterCount` exists only under
    /// `MORPH_BUILD_OFFLINE_SQLITE`. Reporting that as "reads a property the
    /// bridge does not have" would be backwards -- the QML is handling the
    /// absence correctly, and the only way to satisfy the audit would be to
    /// delete the guard that makes it safe.
    QSet<QString> optionalProbes;
};

/// @brief Strips comments and string-literal bodies from @p source, keeping
///        every newline so line numbers survive.
///
/// Exposed for the self-test. The rungs' QML carries long prose comments that
/// quote real binding sites (`page.bookmarkController.refresh()` appears in
/// several), so scanning raw text would both invent references that do not
/// exist and let a comment satisfy the unreferenced-member sweep.
///
/// @param source The `.qml` file's text.
/// @return @p source with `//`, `/* */`, `"`, `'` and backtick spans blanked.
[[nodiscard]] QString blankCommentsAndStrings(const QString& source);

/// @brief Extracts every bridge reference from one already-read `.qml` file.
/// @param source   The file's text; comments and strings are blanked inside.
/// @param fileName Basename recorded on each reference, for the findings.
/// @param aliases  The alias names to look for. Nothing else is scanned:
///        an alias the audit was not told about produces no references.
/// @return The references found, grouped by alias, plus the `Connections`
///         targets seen.
[[nodiscard]] QmlScanResult scanQml(const QString& source, const QString& fileName, const QStringList& aliases);

/// @brief Audits a rung's bridges against the `.qml` that binds them.
///
/// Usage is three lines in a rung's bridge suite:
/// @code
/// QmlSurfaceAudit audit{QStringLiteral(MORPH_LADDER_SOURCE_ROOT "/examples/bookmarks/gui/qml")};
/// audit.bind(QStringLiteral("bookmarkController"), bookmarkBridge);
/// const QStringList findings = audit.run();
/// INFO(findings.join(QStringLiteral("\n")).toStdString());
/// CHECK(findings.isEmpty());
/// @endcode
///
/// The QML is read from the source tree at run time (`MORPH_LADDER_SOURCE_ROOT`
/// is already compiled into every rung's test binary by `morph_add_rung()`),
/// not from the QML module's resources — so the audit needs no QML engine, no
/// `Qt6::Quick`, and runs in configures built without `MORPH_BUILD_FORMS_QML`
/// where the engine-load smoke test compiles to nothing at all.
class QmlSurfaceAudit {
public:
    /// @brief Audits against the `.qml` files under one directory.
    /// @param qmlDirectory Absolute path of a directory whose `*.qml` files
    ///        are the expectation. Searched recursively.
    explicit QmlSurfaceAudit(QString qmlDirectory);

    /// @brief Adds another directory of `.qml` files to the scan.
    /// @param qmlDirectory Absolute path; searched recursively.
    void addDirectory(const QString& qmlDirectory);

    /// @brief Declares that @p alias names @p bridge in every scanned file.
    /// @param alias  The QML property name the shell supplies the bridge as,
    ///        e.g. `"bookmarkController"`.
    /// @param bridge The live instance whose metaobject is the surface.
    void bind(const QString& alias, const QObject& bridge);

    /// @brief Declares that @p alias names @p bridge only in @p fileName.
    ///
    /// Needed where one alias means different bridges in different files —
    /// `ledger`'s sub-views each call their own bridge `bridge`.
    /// @param fileName Basename of the `.qml` file, e.g. `"LedgerView.qml"`.
    /// @param alias    The QML property name within that file.
    /// @param bridge   The live instance whose metaobject is the surface.
    void bindIn(const QString& fileName, const QString& alias, const QObject& bridge);

    /// @brief Exempts one member from the unreferenced-member sweep.
    ///
    /// For surface that is real but bound from QML this audit does not scan —
    /// the shipped `MorphForms` renderer's own files, most often — or for a
    /// pre-existing backlog being recorded rather than swallowed. The reason
    /// is required and is echoed in the audit's summary, so an exemption
    /// cannot be a silent one.
    ///
    /// The list is itself checked: an exemption naming a member the bridge no
    /// longer has, an alias no bridge is bound to, or a member some scanned
    /// `.qml` *does* bind is a finding in its own right. An exemption
    /// therefore cannot outlive the reason it was added.
    /// @param alias  The alias the member belongs to.
    /// @param member Member name, without parentheses or parameter list.
    /// @param reason Why no scanned `.qml` file references it.
    void allowUnbound(const QString& alias, const QString& member, const QString& reason);

    /// @brief Scans, resolves and sweeps.
    /// @return One human-readable line per finding, empty when the surfaces
    ///         agree. Every line names the alias, the member and — for a QML
    ///         reference — the file and line it was written on.
    [[nodiscard]] QStringList run() const;

    /// @brief What `run()` reads, so a test can assert the audit was not
    ///        vacuous itself — a moved or renamed `.qml` file is otherwise
    ///        indistinguishable from a clean audit.
    /// @return Basenames of every `.qml` file found under the directories,
    ///         sorted.
    [[nodiscard]] QStringList scannedFiles() const;

private:
    struct Binding {
        QString alias;
        QString file;  ///< Empty when the binding applies to every file.
        const QObject* bridge = nullptr;
    };

    struct Exemption {
        QString alias;
        QString member;
        QString reason;
    };

    QStringList _directories;
    QVector<Binding> _bindings;
    QVector<Exemption> _exemptions;
};

}  // namespace morph::ladder::testkit
