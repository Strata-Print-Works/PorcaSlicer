# Project tabs

Porca retains several projects in one native window. A project tab selects a complete editing workspace; Prepare and Preview are views of that workspace. Each workspace retains its models, plate/stage setups, camera, selection, undo history, settings controls and slice results. Switching projects changes the active editing context without reloading a project file.

The selected tab joins the workspace with an open lower edge and a coral top marker. Tabs keep a bounded width, ellipsize long titles and show unsaved changes as a dot beside the close action. Full paths remain available in tooltips. New project stays beside the visible tabs; the project list remains at the right edge when tabs overflow.

## Ownership

`ProjectTabs` owns the window's project collection and coordinates activation, opening, closing, save conflicts, recovery and slice scheduling. Each retained `Plater` owns its existing model/plate editor and keeps a shared ownership reference to its preset bundle. Its settings controls are retained with the workspace because `Tab` and field callbacks hold pointers into edited preset collections. Installed profile files remain in the normal local catalog; imported profiles and resolved edits are isolated in each workspace's bundle.

`MainFrame::m_plater`, `GUI_App::plater_`, the active preset bundle, settings panels and tab lists identify the selected editing context. They are changed together during activation. Deferred work must retain its originating workspace rather than resolving these pointers later. A monotonically increasing live session identity distinguishes reopening the same file from continuing the original project. Plugin workspace revisions include that identity.

Devices, Library and the other shared mode pages remain window-owned. A project retains its last project-specific view while a shared page is visible. Design has a retained lazy page per project; `Lazy::make_current` selects which holder its legacy static accessor reaches without constructing or resetting it.

Activation commits pending ordinary field edits, captures the outgoing plate setup, stops its incidental refresh timers and checkpoints unsaved work. Active gestures or UI jobs must finish before switching. The incoming workspace is rebound to Prepare and Preview through `Notebook::ReplacePageWindow`; neither retained workspace is deleted. Model placement and the camera are not fitted or reset.

Only the active workspace registers a 3D mouse controller. Its registration is released on suspension and restored on activation; destroying an inactive workspace cannot disconnect the active controller. Native workspace construction stays hidden until its private state, views and controls exist. Background scheduling is suppressed during construction and activation.

## Files and closure

New creates a workspace. Explicit Open and recent-project actions open another tab; filesystem-equivalent paths select an existing tab. Model and plate imports retain the invoking project as their destination. G-code opens in a separate viewer workspace. A candidate with a failed or cancelled load is disposed while the preceding project remains available. Successful loading is independent of object count, so empty independent plates and Design recipes are valid project contents.

Save As cannot overwrite a file owned by another open project. Saving a known source also compares its modification time and size with the last successful open/save, asking before replacing a changed external file. A save error leaves the project dirty. Save All processes dirty projects in tab order and stops on cancellation or failure.

Closing a tab resolves that project's unsaved changes and slicing work. Decisions for application quit are collected before destroying projects, so a later cancellation leaves the tabs open. Disposal stops workers, unbinds canvases, removes the project's lazy Design holder and releases its recovery files. Closing the last tab creates an empty workspace.

## Slicing

`ProjectSliceQueue` orders requests across projects and prevents duplicate requests for one project. Tickets include a generation, so a completion from cancelled work cannot consume a replacement request. The coordinator waits for the current worker before starting another project's request. Each project retains its existing `PlateSliceBatch` for plate/stage/project scope.

`BackgroundSlicingProcess` retains an explicit workspace owner. Events and UI round trips target that owner; filament mapping reads the captured job configuration rather than the visible preset editor. Slice progress updates the worker's target plate. Hidden completions retain results and defer scene refresh rather than updating another workspace's controls.

A hidden batch advances using captured plate setups and a temporary preset bundle to resolve the next plate's print input. It does not select a plate or load settings into the visible workspace. Thumbnail rendering receives the originating model, plate list and material colors explicitly; GL execution still uses the application's shared context on the UI thread.

Plugin-dependent hidden slicing requires further project-context isolation: a hidden plate that declares plugins is stopped with a review message rather than bypassing plugin checks. Slicing on a selected workspace retains the existing native plugin validation. The slicer and plugin pipelines must not be treated as qualified for every background workflow merely because the queue tests pass.

## Recovery

Non-model dirty flags are keyed by project identity in `ProjectBackupState`, with separate save and backup baselines. Removing one model's backup filters only tasks owned by its backup directory; it cannot clear another project's pending tasks.

Each project checkpoint is a complete `.3mf` copy in its own generated directory under the local `ProjectSessions` folder. The copy is written to a temporary archive and renamed only after export succeeds. The versioned `project-session.json` manifest records source paths, checkpoint paths, tab order, active project, dirty state and the last project view. Unknown or damaged manifests and recovery files are retained rather than reset.

The local view record includes camera target, rotation and zoom, selected plate and object instances. Restoration validates ranges and finite camera values before applying them. It does not use rendered-volume indices, which can change when an archive is loaded. The startup preference to reopen projects restores saved source files after a clean quit; discarded projects are omitted. Crash recovery always uses the last complete checkpoint for unsaved work.

Crash recovery offers projects independently. A damaged checkpoint does not prevent recovery of other projects. Checkpoints load as normal archives, then regain their intended source filename and dirty state. Active slicing and printer delivery are never resumed from the manifest. Live undo and slice caches remain workspace-owned; recovery across process restarts does not restore full undo history or guarantee usable preview assets.

Before a language-driven GUI rebuild, complete project copies are captured for every workspace. The replacement GUI restores those copies without asking crash-recovery questions. If an active edit or export prevents checkpointing, reconstruction is refused while the open work remains available.

Recent-project thumbnails load from a local preview cache at startup. The application refreshes that cache after opening or saving the source archive. An unavailable recent source therefore does not block startup while thumbnail loading waits on that file.

## Verification boundary

`[ProjectBackupState]` and `[ProjectSliceQueue]` exercise independent baselines, FIFO scheduling, cancellation and stale tickets without wx, GL or sockets. They run in the `quick` group. The existing persistence, workspace and preview groups cover their respective format and geometry contracts. Native tab interaction, rendered thumbnails, focus and GL lifetime require application review; other platforms require their own builds and visual checks.
