#ifndef RDEBUGGER_DEBUGSERVER_UI_CONSOLEUI_H_
#define RDEBUGGER_DEBUGSERVER_UI_CONSOLEUI_H_

#include "IDebuggerUI.h"

#include <boost/thread.hpp>
#include <boost/atomic.hpp>

namespace SketchUp {
namespace RubyDebugger {

class ConsoleUI : public IDebuggerUI
{
public:
  ConsoleUI();

  virtual void Initialize(IDebugServer* server);

  virtual void WaitForContinue();

  virtual void Break(BreakPoint bp);

  virtual void Break(const std::string& file, size_t line);

private:
  void ConsoleThreadFunc();
  bool EvaluateCommand(const std::string& str_command);
  void WritePrompt();
  void WriteFrames();

private:
  boost::thread console_thread_;
  bool server_will_continue_;

  boost::mutex console_output_mutex_;

  boost::condition_variable server_wait_cv_;
  boost::mutex server_wait_mutex_;
  bool server_can_continue_;

  boost::atomic<bool> need_server_response_;
  std::string expression_to_evaluate_;
};

} // end namespace RubyDebugger
} // end namespace SketchUp

#endif // RDEBUGGER_DEBUGSERVER_UI_CONSOLEUI_H_