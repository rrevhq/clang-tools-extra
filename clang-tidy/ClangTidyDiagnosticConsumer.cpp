//===--- tools/extra/clang-tidy/ClangTidyDiagnosticConsumer.cpp ----------=== //
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
///  \file This file implements ClangTidyDiagnosticConsumer, ClangTidyMessage,
///  ClangTidyContext and ClangTidyError classes.
///
///  This tool uses the Clang Tooling infrastructure, see
///    http://clang.llvm.org/docs/HowToSetupToolingForLLVM.html
///  for details on setting it up with LLVM source tree.
///
//===----------------------------------------------------------------------===//

#include "ClangTidyDiagnosticConsumer.h"
#include "ClangTidyOptions.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/DiagnosticRenderer.h"
#include "llvm/ADT/SmallString.h"
#include <set>
#include <tuple>
using namespace clang;
using namespace tidy;

namespace {
class ClangTidyDiagnosticRenderer : public DiagnosticRenderer {
public:
  ClangTidyDiagnosticRenderer(const LangOptions &LangOpts,
                              DiagnosticOptions *DiagOpts,
                              ClangTidyError &Error)
      : DiagnosticRenderer(LangOpts, DiagOpts), Error(Error) {}

protected:
  void emitDiagnosticMessage(SourceLocation Loc, PresumedLoc PLoc,
                             DiagnosticsEngine::Level Level, StringRef Message,
                             ArrayRef<CharSourceRange> Ranges,
                             const SourceManager *SM,
                             DiagOrStoredDiag Info) override {
    // Remove check name from the message.
    // FIXME: Remove this once there's a better way to pass check names than
    // appending the check name to the message in ClangTidyContext::diag and
    // using getCustomDiagID.
    std::string CheckNameInMessage = " [" + Error.CheckName + "]";
    if (Message.endswith(CheckNameInMessage))
      Message = Message.substr(0, Message.size() - CheckNameInMessage.size());

    ClangTidyMessage TidyMessage = Loc.isValid()
                                       ? ClangTidyMessage(Message, *SM, Loc)
                                       : ClangTidyMessage(Message);
    if (Level == DiagnosticsEngine::Note) {
      Error.Notes.push_back(TidyMessage);
      return;
    }
    assert(Error.Message.Message.empty() &&
           "Overwriting a diagnostic message");
    Error.Message = TidyMessage;
  }

  void emitDiagnosticLoc(SourceLocation Loc, PresumedLoc PLoc,
                         DiagnosticsEngine::Level Level,
                         ArrayRef<CharSourceRange> Ranges,
                         const SourceManager &SM) override {}

  void emitCodeContext(SourceLocation Loc, DiagnosticsEngine::Level Level,
                       SmallVectorImpl<CharSourceRange> &Ranges,
                       ArrayRef<FixItHint> Hints,
                       const SourceManager &SM) override {
    assert(Loc.isValid());
    for (const auto &FixIt : Hints) {
      CharSourceRange Range = FixIt.RemoveRange;
      assert(Range.getBegin().isValid() && Range.getEnd().isValid() &&
             "Invalid range in the fix-it hint.");
      assert(Range.getBegin().isFileID() && Range.getEnd().isFileID() &&
             "Only file locations supported in fix-it hints.");

      Error.Fix.insert(tooling::Replacement(SM, Range, FixIt.CodeToInsert));
    }
  }

  void emitIncludeLocation(SourceLocation Loc, PresumedLoc PLoc,
                           const SourceManager &SM) override {}

  void emitImportLocation(SourceLocation Loc, PresumedLoc PLoc,
                          StringRef ModuleName,
                          const SourceManager &SM) override {}

  void emitBuildingModuleLocation(SourceLocation Loc, PresumedLoc PLoc,
                                  StringRef ModuleName,
                                  const SourceManager &SM) override {}

  void endDiagnostic(DiagOrStoredDiag D,
                     DiagnosticsEngine::Level Level) override {
    assert(!Error.Message.Message.empty() && "Message has not been set");
  }

private:
  ClangTidyError &Error;
};
} // end anonymous namespace

ClangTidyMessage::ClangTidyMessage(StringRef Message)
    : Message(Message), FileOffset(0) {}

ClangTidyMessage::ClangTidyMessage(StringRef Message,
                                   const SourceManager &Sources,
                                   SourceLocation Loc)
    : Message(Message) {
  assert(Loc.isValid() && Loc.isFileID());
  FilePath = Sources.getFilename(Loc);
  FileOffset = Sources.getFileOffset(Loc);
}

ClangTidyError::ClangTidyError(StringRef CheckName,
                               ClangTidyError::Level DiagLevel)
    : CheckName(CheckName), DiagLevel(DiagLevel) {}

// Returns true if GlobList starts with the negative indicator ('-'), removes it
// from the GlobList.
static bool ConsumeNegativeIndicator(StringRef &GlobList) {
  if (GlobList.startswith("-")) {
    GlobList = GlobList.substr(1);
    return true;
  }
  return false;
}
// Converts first glob from the comma-separated list of globs to Regex and
// removes it and the trailing comma from the GlobList.
static llvm::Regex ConsumeGlob(StringRef &GlobList) {
  StringRef Glob = GlobList.substr(0, GlobList.find(','));
  GlobList = GlobList.substr(Glob.size() + 1);
  llvm::SmallString<128> RegexText("^");
  StringRef MetaChars("()^$|*+?.[]\\{}");
  for (char C : Glob) {
    if (C == '*')
      RegexText.push_back('.');
    else if (MetaChars.find(C) != StringRef::npos)
      RegexText.push_back('\\');
    RegexText.push_back(C);
  }
  RegexText.push_back('$');
  return llvm::Regex(RegexText);
}

GlobList::GlobList(StringRef Globs)
    : Positive(!ConsumeNegativeIndicator(Globs)),
      Regex(ConsumeGlob(Globs)),
      NextGlob(Globs.empty() ? nullptr : new GlobList(Globs)) {}

bool GlobList::contains(StringRef S, bool Contains) {
  if (Regex.match(S))
    Contains = Positive;

  if (NextGlob)
    Contains = NextGlob->contains(S, Contains);
  return Contains;
}

ClangTidyContext::ClangTidyContext(ClangTidyOptionsProvider *OptionsProvider)
    : DiagEngine(nullptr), OptionsProvider(OptionsProvider) {
  // Before the first translation unit we can get errors related to command-line
  // parsing, use empty string for the file name in this case.
  setCurrentFile("");
}

DiagnosticBuilder ClangTidyContext::diag(
    StringRef CheckName, SourceLocation Loc, StringRef Description,
    DiagnosticIDs::Level Level /* = DiagnosticIDs::Warning*/) {
  assert(Loc.isValid());
  bool Invalid;
  const char *CharacterData =
      DiagEngine->getSourceManager().getCharacterData(Loc, &Invalid);
  if (!Invalid) {
    const char *P = CharacterData;
    while (*P != '\0' && *P != '\r' && *P != '\n')
      ++P;
    StringRef RestOfLine(CharacterData, P - CharacterData + 1);
    // FIXME: Handle /\bNOLINT\b(\([^)]*\))?/ as cpplint.py does.
    if (RestOfLine.find("NOLINT") != StringRef::npos) {
      Level = DiagnosticIDs::Ignored;
      ++Stats.ErrorsIgnoredNOLINT;
    }
  }
  unsigned ID = DiagEngine->getDiagnosticIDs()->getCustomDiagID(
      Level, (Description + " [" + CheckName + "]").str());
  if (CheckNamesByDiagnosticID.count(ID) == 0)
    CheckNamesByDiagnosticID.insert(std::make_pair(ID, CheckName.str()));
  return DiagEngine->Report(Loc, ID);
}

void ClangTidyContext::setDiagnosticsEngine(DiagnosticsEngine *Engine) {
  DiagEngine = Engine;
}

void ClangTidyContext::setSourceManager(SourceManager *SourceMgr) {
  DiagEngine->setSourceManager(SourceMgr);
}

void ClangTidyContext::setCurrentFile(StringRef File) {
  CurrentFile = File;
  CheckFilter.reset(new GlobList(getOptions().Checks));
}

void ClangTidyContext::setASTContext(ASTContext *Context) {
  DiagEngine->SetArgToStringFn(&FormatASTNodeDiagnosticArgument, Context);
}

const ClangTidyGlobalOptions &ClangTidyContext::getGlobalOptions() const {
  return OptionsProvider->getGlobalOptions();
}

const ClangTidyOptions &ClangTidyContext::getOptions() const {
  return OptionsProvider->getOptions(CurrentFile);
}

GlobList &ClangTidyContext::getChecksFilter() {
  assert(CheckFilter != nullptr);
  return *CheckFilter;
}

/// \brief Store a \c ClangTidyError.
void ClangTidyContext::storeError(const ClangTidyError &Error) {
  Errors.push_back(Error);
}

StringRef ClangTidyContext::getCheckName(unsigned DiagnosticID) const {
  llvm::DenseMap<unsigned, std::string>::const_iterator I =
      CheckNamesByDiagnosticID.find(DiagnosticID);
  if (I != CheckNamesByDiagnosticID.end())
    return I->second;
  return "";
}

ClangTidyDiagnosticConsumer::ClangTidyDiagnosticConsumer(ClangTidyContext &Ctx)
    : Context(Ctx), LastErrorRelatesToUserCode(false),
      LastErrorPassesLineFilter(false) {
  IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts = new DiagnosticOptions();
  Diags.reset(new DiagnosticsEngine(
      IntrusiveRefCntPtr<DiagnosticIDs>(new DiagnosticIDs), &*DiagOpts, this,
      /*ShouldOwnClient=*/false));
  Context.setDiagnosticsEngine(Diags.get());
}

void ClangTidyDiagnosticConsumer::finalizeLastError() {
  if (!Errors.empty()) {
    ClangTidyError &Error = Errors.back();
    if (!Context.getChecksFilter().contains(Error.CheckName) &&
        Error.DiagLevel != ClangTidyError::Error) {
      ++Context.Stats.ErrorsIgnoredCheckFilter;
      Errors.pop_back();
    } else if (!LastErrorRelatesToUserCode) {
      ++Context.Stats.ErrorsIgnoredNonUserCode;
      Errors.pop_back();
    } else if (!LastErrorPassesLineFilter) {
      ++Context.Stats.ErrorsIgnoredLineFilter;
      Errors.pop_back();
    } else {
      ++Context.Stats.ErrorsDisplayed;
    }
  }
  LastErrorRelatesToUserCode = false;
  LastErrorPassesLineFilter = false;
}

void ClangTidyDiagnosticConsumer::HandleDiagnostic(
    DiagnosticsEngine::Level DiagLevel, const Diagnostic &Info) {
  if (DiagLevel == DiagnosticsEngine::Note) {
    assert(!Errors.empty() &&
           "A diagnostic note can only be appended to a message.");
  } else {
    finalizeLastError();
    StringRef WarningOption =
        Context.DiagEngine->getDiagnosticIDs()->getWarningOptionForDiag(
            Info.getID());
    std::string CheckName = !WarningOption.empty()
                                ? ("clang-diagnostic-" + WarningOption).str()
                                : Context.getCheckName(Info.getID()).str();

    if (CheckName.empty()) {
      // This is a compiler diagnostic without a warning option. Assign check
      // name based on its level.
      switch (DiagLevel) {
        case DiagnosticsEngine::Error:
        case DiagnosticsEngine::Fatal:
          CheckName = "clang-diagnostic-error";
          break;
        case DiagnosticsEngine::Warning:
          CheckName = "clang-diagnostic-warning";
          break;
        default:
          CheckName = "clang-diagnostic-unknown";
          break;
      }
    }

    ClangTidyError::Level Level = ClangTidyError::Warning;
    if (DiagLevel == DiagnosticsEngine::Error ||
        DiagLevel == DiagnosticsEngine::Fatal) {
      // Force reporting of Clang errors regardless of filters and non-user
      // code.
      Level = ClangTidyError::Error;
      LastErrorRelatesToUserCode = true;
      LastErrorPassesLineFilter = true;
    }
    Errors.push_back(ClangTidyError(CheckName, Level));
  }

  // FIXME: Provide correct LangOptions for each file.
  LangOptions LangOpts;
  ClangTidyDiagnosticRenderer Converter(
      LangOpts, &Context.DiagEngine->getDiagnosticOptions(), Errors.back());
  SmallString<100> Message;
  Info.FormatDiagnostic(Message);
  SourceManager *Sources = nullptr;
  if (Info.hasSourceManager())
    Sources = &Info.getSourceManager();
  Converter.emitDiagnostic(Info.getLocation(), DiagLevel, Message,
                           Info.getRanges(), Info.getFixItHints(), Sources);

  checkFilters(Info.getLocation());
}

void ClangTidyDiagnosticConsumer::BeginSourceFile(const LangOptions &LangOpts,
                                                  const Preprocessor *PP) {
  // Before the first translation unit we don't need HeaderFilter, as we
  // shouldn't get valid source locations in diagnostics.
  HeaderFilter.reset(new llvm::Regex(Context.getOptions().HeaderFilterRegex));
}

bool ClangTidyDiagnosticConsumer::passesLineFilter(StringRef FileName,
                                                   unsigned LineNumber) const {
  if (Context.getGlobalOptions().LineFilter.empty())
    return true;
  for (const FileFilter& Filter : Context.getGlobalOptions().LineFilter) {
    if (FileName.endswith(Filter.Name)) {
      if (Filter.LineRanges.empty())
        return true;
      for (const FileFilter::LineRange &Range : Filter.LineRanges) {
        if (Range.first <= LineNumber && LineNumber <= Range.second)
          return true;
      }
      return false;
    }
  }
  return false;
}

void ClangTidyDiagnosticConsumer::checkFilters(SourceLocation Location) {
  // Invalid location may mean a diagnostic in a command line, don't skip these.
  if (!Location.isValid()) {
    LastErrorRelatesToUserCode = true;
    LastErrorPassesLineFilter = true;
    return;
  }

  const SourceManager &Sources = Diags->getSourceManager();
  if (Sources.isInSystemHeader(Location))
    return;

  // FIXME: We start with a conservative approach here, but the actual type of
  // location needed depends on the check (in particular, where this check wants
  // to apply fixes).
  FileID FID = Sources.getDecomposedExpansionLoc(Location).first;
  const FileEntry *File = Sources.getFileEntryForID(FID);

  // -DMACRO definitions on the command line have locations in a virtual buffer
  // that doesn't have a FileEntry. Don't skip these as well.
  if (!File) {
    LastErrorRelatesToUserCode = true;
    LastErrorPassesLineFilter = true;
    return;
  }

  StringRef FileName(File->getName());
  assert(LastErrorRelatesToUserCode || Sources.isInMainFile(Location) ||
         HeaderFilter != nullptr);
  LastErrorRelatesToUserCode = LastErrorRelatesToUserCode ||
                               Sources.isInMainFile(Location) ||
                               HeaderFilter->match(FileName);

  unsigned LineNumber = Sources.getExpansionLineNumber(Location);
  LastErrorPassesLineFilter =
      LastErrorPassesLineFilter || passesLineFilter(FileName, LineNumber);
}

namespace {
struct LessClangTidyError {
  bool operator()(const ClangTidyError *LHS, const ClangTidyError *RHS) const {
    const ClangTidyMessage &M1 = LHS->Message;
    const ClangTidyMessage &M2 = RHS->Message;

    return std::tie(M1.FilePath, M1.FileOffset, M1.Message) <
           std::tie(M2.FilePath, M2.FileOffset, M2.Message);
  }
};
} // end anonymous namespace

// Flushes the internal diagnostics buffer to the ClangTidyContext.
void ClangTidyDiagnosticConsumer::finish() {
  finalizeLastError();
  std::set<const ClangTidyError*, LessClangTidyError> UniqueErrors;
  for (const ClangTidyError &Error : Errors)
    UniqueErrors.insert(&Error);

  for (const ClangTidyError *Error : UniqueErrors)
    Context.storeError(*Error);
  Errors.clear();
}
