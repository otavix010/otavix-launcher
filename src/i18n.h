// The launcher's own words in Portuguese, English and Spanish; the game's come from its profile.
#pragma once
#include "profile.h"

namespace launcher {

enum class S {
    Play, Playing, Home, Saves, About, Settings, Language, LanguageAuto,
    GameFound, GameMissing, GameMissingHelp, PickFolder, PickFile, Change, Clear,
    Presets, PresetsHelp, Apply, Default, Desktop, Internal, Unsaved, SavedOk, SaveFailed,
    ButtonPress, ButtonWaiting, ButtonCancel, ResetButtons, Controllers, NoControllers,
    SavesHelp, SaveFolder, Empty, BackupNow, BackupAuto, Backups, Restore, RestoreConfirm, OpenFolder, NoBackups,
    BackupDone, RestoreDone, Yes, No, Logs, OpenLogs, LastExit, ExitCode, Credits, Legal, Version,
    HintSelect, HintBack, HintPages, NoProfile, StartFailed, Running, Minutes,
    Advanced, HintTabs, ActivePreset, PresetsTitle, Custom,
    AllGames, Library, LauncherSettings, Exit, BackToLibrary, NoGames, Wip, GamesCount, OneGame,
    General, Updates, WindowMode, Windowed, Fullscreen, WindowSize, UiScale, OnPlay, OnPlayMinimize, OnPlayKeep,
    OnPlayClose, BgAnim, StartLast, CheckOnStart, CheckNow, Checking, UpToDate, NewVersion, OpenRelease, NoRepo,
    UpdateFailed, Installed, HubAbout, HubLegal, HintOpen, MonitorModes,
    PlatformWindows, PlatformLinux, Soon, Available, HintPlatform, OtherPlatform, OtherPlatformHelp, Version2,
    Report, ReportIntro, ReportCategory, CatCrash, CatGraphics, CatAudio, CatInput, CatPerf, CatOther, ReportDesc, ReportContact, ReportAttach, ReportPreview, ReportSend, Sending, ReportSent, SendFailed, ReportSaved, ServerOff, ReportEmpty, OpenReports, Requests, RequestsIntro, Console, GameName, Comment, Request, Requested, AlreadyRequested, MostRequested, Week, Month, AllTime, AllConsoles, Votes, NoRequests, RequestsOff, Loading, Admin, AdminKey, AdminHelp, Hide, Show, Rename, Merge, Hidden, Comments, RecentReports, Refresh, ThemeMusic, MusicVolume, Save, Cancel, Mailed, SearchHint, NotInList, RequestTyped, TypedName, BackToList, OneVote, Mods, ModsHelp, OpenModsFolder, InstallZip, NoMods, ModFiles, ModInstall, ModRemove, ModRemoveConfirm, ModConflicts, ModConflictFiles, ModsOff, Installing, ModInstalled, ModInstallFailed, UseMods, ModsGeneralHelp, ModsCount, Manage, NoModGames, Tutorial, TutorialIntro, Tips, Faq, InstallGame, HaveFolder, ChangeGame, GameNotInstalled, InstallHelp, NeededVersion, Unpacking, CheckingVersion, ReadingArchive, VersionOk, VersionOther, DeleteArchive, DeleteArchiveConfirm, ArchiveDeleted, InstallFailed, DesktopShortcut, ShortcutDone, ShortcutFailed, InstallAuto, InstallAutoText, InstallManual, InstallManualText, ManualSteps, ManualStep1, ManualStep2, ManualStep3, PickImage, PortMissing, PortMissingText, DownloadPort, DownloadPortNoSize, DownloadingPort, UnpackingPort, PortFailed, PortNewer, UpdatePort, PortUpdated, UpdateNow, WhatsNew, DownloadingUpdate, InstallingUpdate, LauncherNewer, CrashTitle, CrashText, SendToGitHub, Dismiss, CloseHint, CloseCombo, InstallFile, InstallFileText, BuildTitle, BuildIntro, BuildNeeds, PrereqOk, PrereqMissing, PrereqChecking, PrereqInstall, PrereqRecheck, BuildInputs, PickFolderBtn, PickFileBtn, NotChosen, BuildStart, BuildRunning, BuildMinimize, BuildFailedTitle, BuildDoneTitle, BuildNewCode, BuildRedo,
    Perf, PerfThisPc, PerfMine, PerfNone, PerfCommunity, PerfCommunityIntro, PerfServerOff, PerfCommunityEmpty, PerfDate, PerfAvg, PerfLow, PerfMin, PerfOptions, PerfSessions, PerfCpu, PerfGpu, PerfRam, PerfShare, PerfShareHelp, PerfAskTitle, PerfAskText, PerfShowWhat, PerfSend, PerfDontSend, PerfLastReport, PerfNoReport, PerfDeleteLocal, PerfDeleteServer, PerfDeleted, PerfMachines, UpdPopTitle, UpdPopLauncher, UpdLater, UpdIgnore, UpdPortsTitle, UpdPortsText, UpdOpen, UpdIgnorePorts, PerfOsd, PerfOsdHelp, WhatsNewTitle, NotesNone, NotesInstalled, NotesIntro, SearchGames, SearchYours, SearchNoPort, SearchNothing, SearchMore, RequestPort, RequestedName, AlreadyName, RequestedMark, HintSearch,
    AnswerTitle, AnswerYourReport, AnswerOk, AnswersHere, AnswerBtn, AnswerLabel, AnswerSend, Answered,
    IssueCopied, IssueNoBrowser, IssueNoBrowserReport, UnpackHelp, UnpackFailed,
    PortUpdates, CheckPortsNow, PortUpToDate, NoPortsInstalled,
    ModsPsxIntro, ModsPsxGame, ModsPsxFramework, ModsPsxNone,
    UpdateAll, UpdateAllHelp, UpdateAllPorts, UpdateAllGames, UpdateAllWaiting, UpdateAllDone, UpdateAllStopped,
    UpdateAllFailed, UpdateAllCurrent, UpdateAllNeedsInputs, UpdateAllDoneErrors,
    ReportPhotos, ReportPhotosHelp, ReportAddPhotos, ReportPastePhoto, ReportImages, ReportNoClipboard, ReportPhotoLimit,
    ReportSendingPhotos, ReportPhotosSent, ReportPhotosFailed, RemoveRequest, RemoveRequestConfirm, MyRequests,
    WithdrawRequest, RequestWithdrawn, FilterAll, FilterPlayable, FilterWip,
    FilterInstalled, HintFilter,
    BuildCopyMissing, BuildCopyMissingHelp, BuildRepick,
    DiscsFolder, DiscsFolderHelp, DiscFoundThere, DiscFoundAt, UseFoundDisc, PickedPortItself, PickedPortsFolder,
    GamesFolder, GamesFolderHelp, GamesFolderAuto, GamesFolderBad,
    GameFolderAt, ChangeFolder, GameFolderBusy, GameFolderNotEmpty, GameFolderMoveFailed,
    ReportToLauncher,
    StaleTitle, StaleText, StaleBuildNow, StalePlayOld,
    News, NewsIntro, NewsInstalled, NewsInUpdate, NewsNone, NewsRebuild,
    Count
};

const char* T(Lang l, S s);
Lang SystemLang();

}  // namespace launcher
