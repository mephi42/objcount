#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS

#include <map>
#include <sstream>

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/FileManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendPluginRegistry.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Rewrite/Rewriter.h>
#include <llvm/Support/raw_ostream.h>

class Consumer: public clang::ASTConsumer {
public:
  Consumer( clang::CompilerInstance& compiler ) :
    verbose_( true ),
    rewriter_( compiler.getPreprocessor().getSourceManager(),
               compiler.getPreprocessor().getLangOptions() ) {
  }

  virtual void HandleTranslationUnit( clang::ASTContext& context ) override {
    // Log context address.
    if( verbose_ ) {
      llvm::errs() << "context = " << &context << "\n";
    }

    // Process all declarations recursively starting from translation unit.
    processDecl( *context.getTranslationUnitDecl(), "" );

    // Find main file.
    auto& sourceManager = rewriter_.getSourceMgr();
    auto mainFileId = sourceManager.getMainFileID();
    auto mainFileEntry = sourceManager.getFileEntryForID( mainFileId );

    // Add #include.
    auto start = sourceManager.getLocForStartOfFile( mainFileId );
    if( rewriter_.InsertTextAfter( start, "#include \"Countable.hpp\"\n" ) ) {
      if( verbose_ ) {
        llvm::errs() << "failed to add include\n";
      }
      return;
    }

    auto mainFileBuf = rewriter_.getRewriteBufferFor( mainFileId );
    if( mainFileBuf != NULL ) {
      // Open file being compiled.
      std::string errorString;
      llvm::raw_fd_ostream mainFileStream(
        mainFileEntry->getName(),
        errorString,
        llvm::raw_fd_ostream::F_Binary );
      if( !errorString.empty() ) {
        llvm::errs() << "warning: could not open " <<
                        mainFileEntry->getName() << "\n";
        return;
      }

      // Write instrumented code into the main file.
      mainFileBuf->write( mainFileStream );
      mainFileStream.flush();
    }
  }

private:
  bool acceptClass( clang::CXXRecordDecl& clazz ) {
    return clazz.getDefinition() == &clazz &&
           !clazz.isUnion() &&
           clazz.getDeclName() &&
           !clazz.isAggregate();
  }

  void processDecl( clang::Decl& decl, const std::string& pad ) {
    std::string nextPad = pad + "  ";

    // Log declaration address and kind.
    if( verbose_ ) {
      llvm::errs() << pad << "decl = " << &decl << "\n";
      llvm::errs() << pad << "  kind = " << decl.getDeclKindName() << "\n";
    }

    // Log declaration name.
    auto named = clang::dyn_cast< clang::NamedDecl >( &decl );
    if( named != NULL ) {
      auto name = named->getNameAsString();
      if( !name.empty() ) {
        llvm::errs() << pad << "  name = " << name << "\n";
      }
    }

    // Handle class declaration.
    auto clazz = clang::dyn_cast< clang::CXXRecordDecl >( &decl );
    if( clazz != NULL ) {
      processClass( *clazz, nextPad );
    }

    // Handle class template declaration.
    auto tmpl = clang::dyn_cast< clang::ClassTemplateDecl >( &decl );
    if( tmpl != NULL ) {
      processClass( *tmpl->getTemplatedDecl(), nextPad );
    }

    // Invoke self recursively.
    auto context = clang::dyn_cast< clang::DeclContext >( &decl );
    if( context == NULL ) {
      return;
    }
    for( auto it = context->decls_begin(); it != context->decls_end(); ++it ) {
      processDecl( **it, nextPad );
    }
  }

  void processClass( clang::CXXRecordDecl& clazz, const std::string& pad ) {
    if( !acceptClass( clazz ) ) {
      if( verbose_ ) {
        llvm::errs() << pad << "not accepted\n";
      }
      return;
    }

    if( clang::isa< clang::ClassTemplateSpecializationDecl >( &clazz ) ) {
      if( verbose_ ) {
        llvm::errs() << pad << "TODO: handle template specializations\n";
      }
      return;
    }

    std::ostringstream className;
    auto tmpl = clazz.getDescribedClassTemplate();
    if( tmpl == NULL ) {
      className << clazz.getNameAsString();
    } else {
      className << clazz.getNameAsString() << "<";
      auto parms = tmpl->getTemplateParameters();
      bool first = true;
      for( auto it = parms->begin(); it != parms->end(); ++it ) {
        if( first ) {
          first = false;
        } else {
          className << ", ";
        }
        auto name = (*it)->getDeclName();
        if( name ) {
          className << name.getAsString();
        } else {
          if( verbose_ ) {
            llvm::errs() << pad << "TODO: handle templates with unnamed parameters\n";
          }
          return;
        }
        auto typeParm = clang::dyn_cast< clang::TemplateTypeParmDecl >( (*it) );
        if( typeParm != NULL && typeParm->isParameterPack() ) {
          className << "...";
        }
      }
      className << ">";
    }

    if( clazz.getNumBases() == 0 ) {
      std::ostringstream baseText;
      baseText << ": private Countable<" << className.str() << "> ";

      auto loc = clazz.getLocation();
      if( rewriter_.InsertTextAfterToken( loc,
                                          baseText.str() ) ) {
        if( verbose_ ) {
          llvm::errs() << pad << "failed to add a base class\n";
        }
        return;
      }
      if( verbose_ ) {
        llvm::errs() << pad << "added a base class to ";
        loc.print( llvm::errs(), rewriter_.getSourceMgr() );
        llvm::errs() << "\n";
      }
    } else {
      std::ostringstream baseText;
      baseText << ", private Countable<" << className.str() << "> ";

      auto loc = clazz.bases_begin()->getSourceRange().getEnd();
      if( rewriter_.InsertTextAfterToken( loc,
                                          baseText.str() ) ) {
        if( verbose_ ) {
          llvm::errs() << pad << "failed to add a base class\n";
        }
        return;
      }
      if( verbose_ ) {
        llvm::errs() << pad << "added a base class to ";
        loc.print( llvm::errs(), rewriter_.getSourceMgr() );
        llvm::errs() << "\n";
      }
    }
  }

  bool verbose_;
  clang::Rewriter rewriter_;
};

class Action: public clang::PluginASTAction {
protected:
  virtual clang::ASTConsumer*
  CreateASTConsumer( clang::CompilerInstance& compiler,
                     llvm::StringRef ) override {
    return new Consumer( compiler );
  }

  virtual bool ParseArgs( const clang::CompilerInstance& CI,
                          const std::vector< std::string >& args ) override {
    return true;
  }
};

static clang::FrontendPluginRegistry::Add< Action >
X("objcount", "add object counters");
