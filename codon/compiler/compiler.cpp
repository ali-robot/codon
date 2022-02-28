#include "compiler.h"

#include "codon/parser/cache.h"
#include "codon/parser/peg/peg.h"
#include "codon/parser/visitors/doc/doc.h"
#include "codon/parser/visitors/format/format.h"
#include "codon/parser/visitors/simplify/simplify.h"
#include "codon/parser/visitors/translate/translate.h"
#include "codon/parser/visitors/typecheck/typecheck.h"

namespace codon {
namespace {
ir::transform::PassManager::Init getPassManagerInit(Compiler::Mode mode, bool isTest) {
  using ir::transform::PassManager;
  switch (mode) {
  case Compiler::Mode::DEBUG:
    return isTest ? PassManager::Init::RELEASE : PassManager::Init::DEBUG;
  case Compiler::Mode::RELEASE:
    return PassManager::Init::RELEASE;
  case Compiler::Mode::JIT:
    return PassManager::Init::JIT;
  default:
    return PassManager::Init::EMPTY;
  }
}
} // namespace

Compiler::Compiler(const std::string &argv0, Compiler::Mode mode,
                   const std::vector<std::string> &disabledPasses, bool isTest)
    : argv0(argv0), debug(mode == Mode::DEBUG), input(),
      plm(std::make_unique<PluginManager>()),
      cache(std::make_unique<ast::Cache>(argv0)),
      module(std::make_unique<ir::Module>()),
      pm(std::make_unique<ir::transform::PassManager>(getPassManagerInit(mode, isTest),
                                                      disabledPasses)),
      llvisitor(std::make_unique<ir::LLVMVisitor>()) {
  cache->module = module.get();
  module->setCache(cache.get());
  llvisitor->setDebug(debug);
  llvisitor->setPluginManager(plm.get());
}

llvm::Error Compiler::load(const std::string &plugin) {
  auto result = plm->load(plugin);
  if (auto err = result.takeError())
    return err;

  auto *p = *result;
  if (!p->info.stdlibPath.empty()) {
    cache->pluginImportPaths.push_back(p->info.stdlibPath);
  }
  for (auto &kw : p->dsl->getExprKeywords()) {
    cache->customExprStmts[kw.keyword] = kw.callback;
  }
  for (auto &kw : p->dsl->getBlockKeywords()) {
    cache->customBlockStmts[kw.keyword] = {kw.hasExpr, kw.callback};
  }
  p->dsl->addIRPasses(pm.get(), debug);
  return llvm::Error::success();
}

llvm::Error
Compiler::parse(bool isCode, const std::string &file, const std::string &code,
                int startLine, int testFlags,
                const std::unordered_map<std::string, std::string> &defines) {
  input = file;
  std::string abspath = (file != "-") ? ast::getAbsolutePath(file) : file;
  try {
    Timer t1("parse");
    ast::StmtPtr codeStmt = isCode
                                ? ast::parseCode(cache.get(), abspath, code, startLine)
                                : ast::parseFile(cache.get(), abspath);
    t1.log();

    cache->module0 = file;
    if (testFlags)
      cache->testFlags = testFlags;

    Timer t2("simplify");
    auto transformed = ast::SimplifyVisitor::apply(cache.get(), std::move(codeStmt),
                                                   abspath, defines, (testFlags > 1));
    t2.log();
    if (codon::getLogger().flags & codon::Logger::FLAG_USER) {
      auto fo = fopen("_dump_simplify.sexp", "w");
      fmt::print(fo, "{}\n", transformed->toString(0));
      fclose(fo);
    }

    Timer t3("typecheck");
    auto typechecked =
        ast::TypecheckVisitor::apply(cache.get(), std::move(transformed));
    t3.log();
    if (codon::getLogger().flags & codon::Logger::FLAG_USER) {
      auto fo = fopen("_dump_typecheck.sexp", "w");
      fmt::print(fo, "{}\n", typechecked->toString(0));
      for (auto &f : cache->functions)
        for (auto &r : f.second.realizations)
          fmt::print(fo, "{}\n", r.second->ast->toString(0));
      fclose(fo);
    }

    Timer t4("translate");
    ast::TranslateVisitor::apply(cache.get(), std::move(typechecked));
    t4.log();
  } catch (const exc::ParserException &e) {
    return llvm::make_error<error::ParserErrorInfo>(e);
  }
  module->setSrcInfo({abspath, 0, 0, 0});
  return llvm::Error::success();
}

llvm::Error
Compiler::parseFile(const std::string &file, int testFlags,
                    const std::unordered_map<std::string, std::string> &defines) {
  return parse(/*isCode=*/false, file, /*code=*/"", /*startLine=*/0, testFlags,
               defines);
}

llvm::Error
Compiler::parseCode(const std::string &file, const std::string &code, int startLine,
                    int testFlags,
                    const std::unordered_map<std::string, std::string> &defines) {
  return parse(/*isCode=*/true, file, code, startLine, testFlags, defines);
}

llvm::Error Compiler::compile() {
  pm->run(module.get());
  llvisitor->visit(module.get());
  return llvm::Error::success();
}

llvm::Expected<std::string> Compiler::docgen(const std::vector<std::string> &files) {
  try {
    auto j = ast::DocVisitor::apply(argv0, files);
    return j->toString();
  } catch (exc::ParserException &e) {
    return llvm::make_error<error::ParserErrorInfo>(e);
  }
}

} // namespace codon