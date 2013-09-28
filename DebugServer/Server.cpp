#include "stdafx.h"
#include "Server.h"
#include "UI/IDebuggerUI.h"
#include "Common/BreakPoint.h"
#include "Common/StackFrame.h"
#include "FindSubstringCaseInsensitive.h"

#include <ruby/ruby.h>
#include <ruby/ruby/debug.h>
#include <ruby/ruby/encoding.h>

#include <boost/thread.hpp>
#include <boost/atomic.hpp>

#include <string>
#include <iostream>
#include <map>

using namespace SketchUp::RubyDebugger;

namespace {

VALUE GetRubyInterface(const char* s) {
  VALUE str_val = rb_str_new2(s);
  // Mark all strings as UTF-8 encoded.
  static int enc_index = rb_utf8_encindex();
  rb_enc_associate_index(str_val, enc_index);
  return str_val;
}

std::string GetRubyString(VALUE obj) {
  std::string s;
  if (TYPE(obj) != T_STRING) {
    s = StringValuePtr(obj);
    return s;
  }
  // Grab the string pointer.
  const char* source = RSTRING_PTR(obj);
  // Assign assuming utf8.
  s = source;
  return s;
}

// Get a Ruby object as a string.
std::string GetRubyObjectAsString(VALUE obj) {
  // See if it is a string.
  if (TYPE(obj) == T_STRING) {
    return GetRubyString(obj);
  }
  // Look for true and false.
  std::string objString;
  if (obj == Qtrue) {
    objString = "true";
  } else if (obj == Qfalse) {
    objString = "false";
  } else if (obj == Qnil) {
    objString = "nil";
  } else {
    // See if we can convert it to a string.
    int error = 0;
    VALUE s = rb_protect(rb_obj_as_string, obj, &error);
    if (0 == error && s != Qnil) {
      objString = StringValueCStr(s);
    }
  }
  return objString;
}

int GetRubyInt(VALUE obj) {
  return NUM2INT(obj);
}

// Function for wrap the call to rb_funcall2 so that we will not exit SketchUp
// if there is an error in the Ruby code that we are calling.
VALUE WrapFuncall(VALUE data) {
  // The data is all wrapped in a single Ruby array.
  if (!(TYPE(data) == T_ARRAY)) {
    return Qnil;
  }
  int argc = RARRAY_LEN(data);
  if (argc < 2) {
    return Qnil;
  }
  VALUE* argv = RARRAY_PTR(data);
  VALUE obj = argv[0];
  ID func = (ID) NUM2LONG(argv[1]);

  argc -= 2;
  argv += 2;

  return rb_funcall2(obj, func, argc, argv);
}

VALUE ProtectFuncall(VALUE obj, ID func, int argc, ...) {
  // First check to see if we have a valid object, and if it responds to the
  // desired method.
  if (obj == Qnil) {
    return Qnil;
  }
  if (!rb_respond_to(obj, func)) {
    return Qnil;
  }
  // rb_protect takes a pointer to a function that only takes a single argument
  // so we need to package up all of the arguments into a single array.  The
  // array will contain the object that the function is called on, the ID of the
  // method to call and the arguments to pass to the method.

  // Allocate the array and push on the object and the method ID
  VALUE data = rb_ary_new2(argc + 2);
  rb_ary_push(data, obj);
  rb_ary_push(data, LONG2NUM(func));

  // Now push on all of the arguments.
  if (argc > 0) {
    va_list ar;
    va_start(ar, argc);
    for (int i = 0; i < argc; i++) {
      rb_ary_push(data, va_arg(ar, VALUE));
    }
    va_end(ar);
  }

  // Now call the wrapped function inside a protected context.
  int error;
  VALUE result = rb_protect(WrapFuncall, data, &error);
  if (error) {
    result = rb_errinfo();
  }

  return result;
}

std::string EvaluateRubyExpression(const std::string& expr, VALUE binding) {
  VALUE str_to_eval = GetRubyInterface(expr.c_str());
  static ID eval_method_id = rb_intern("eval");
  VALUE val = ProtectFuncall(rb_mKernel, eval_method_id, 2, str_to_eval,
                             binding);
  return GetRubyObjectAsString(val);
}

VALUE DebugInspectorFunc(const rb_debug_inspector_t* di, void* data) {
  auto frames = reinterpret_cast<std::vector<StackFrame>*>(data);
  VALUE bt = rb_debug_inspector_backtrace_locations(di);
  int bt_count = RARRAY_LEN(bt);
  for (int i=0; i < bt_count; ++i) {
    VALUE bt_val = RARRAY_PTR(bt)[i];
    std::string frame_str = GetRubyObjectAsString(bt_val);
    VALUE binding_val = rb_debug_inspector_frame_binding_get(di, i);
    StackFrame frame;
    frame.name = frame_str;
    frame.binding = binding_val;
    frames->push_back(frame);
  }
  return Qnil;
}

bool SortBreakPoints(const SketchUp::RubyDebugger::BreakPoint& bp0,
                     const SketchUp::RubyDebugger::BreakPoint& bp1) {
  return bp0.index < bp1.index;
}

} // end anonymous namespace

namespace SketchUp {
namespace RubyDebugger {

class Server::Impl {
public:
  Impl()
    : last_breakpoint_index(0),
      script_lines_hash_(0),
      is_stopped_(false),
      break_at_next_line_(false),
      active_frame_index_(0),
      last_break_line_(0)
  {}

  void EnableTracePoint();

  const BreakPoint* GetBreakPoint(const std::string& file, size_t line) const;

  void ReadScriptLinesHash();

  bool ResolveBreakPoint(BreakPoint& bp);

  void ResolveBreakPoints();

  void AddBreakPoint(BreakPoint& bp, bool is_resolved);

  void ClearBreakData();

  static std::vector<StackFrame> GetStackFrames();

  static void TraceFunc(VALUE tp_val, void* data);

  std::unique_ptr<IDebuggerUI> ui_;

  // Breakpoints with yet-unresolved file paths
  std::vector<BreakPoint> unresolved_breakpoints_;

  // map<line, map<file, BreakPoint>>
  std::map<size_t, std::map<std::string, BreakPoint>> breakpoints_;

  size_t last_breakpoint_index;

  boost::mutex break_point_mutex_;

  VALUE script_lines_hash_;

  std::map<std::string, std::vector<std::string>> script_lines_;

  boost::atomic<bool> is_stopped_;

  boost::atomic<bool> break_at_next_line_;
  
  std::vector<StackFrame> frames_;

  size_t active_frame_index_;

  std::string last_break_file_path_;

  size_t last_break_line_;
};

void Server::Impl::ClearBreakData() {
  frames_.clear();
  is_stopped_ = false;
  last_break_file_path_.clear();
  last_break_line_ = 0;
}

void Server::Impl::EnableTracePoint() {
  VALUE tp = rb_tracepoint_new(Qnil, RUBY_EVENT_LINE, &TraceFunc, this);
  rb_tracepoint_enable(tp);
}

const BreakPoint* Server::Impl::GetBreakPoint(const std::string& file,
                                              size_t line) const {
  const BreakPoint* bp = nullptr;
  auto it = breakpoints_.find(line);
  if (it != breakpoints_.end()) {
    auto itf = it->second.find(file);
    if (itf != it->second.end()) {
      bp = &(itf->second);
    }
  }
  return bp;
}

void Server::Impl::TraceFunc(VALUE tp_val, void* data) {
  rb_trace_arg_t* trace_arg = rb_tracearg_from_tracepoint(tp_val);
  Server::Impl* server = reinterpret_cast<Server::Impl*>(data);
  server->ClearBreakData();
  
  VALUE event = rb_tracearg_event(trace_arg);
  ID event_id = SYM2ID(event);
  static const ID id_line = rb_intern("line");
  static const ID id_class = rb_intern("class");
  static const ID id_end = rb_intern("end");
  static const ID id_call = rb_intern("call");
  static const ID id_return = rb_intern("return");
  static const ID id_c_call = rb_intern("c_call");
  static const ID id_c_return = rb_intern("c_return");
  static const ID id_raise = rb_intern("raise");

  std::string file_path = GetRubyString(rb_tracearg_path(trace_arg));
  int line = GetRubyInt(rb_tracearg_lineno(trace_arg));

  if (event_id == id_line) {
    if (server->break_at_next_line_) {
      server->break_at_next_line_ = false;
      server->frames_ = GetStackFrames();
      server->last_break_file_path_ = file_path;
      server->last_break_line_ = line;
      server->is_stopped_ = true;
      server->ui_->Break(file_path, line); // Blocked here until ui says continue
      server->ClearBreakData();
    } else {
      // Try to resolve any unresolved breakpoints
      if (!server->unresolved_breakpoints_.empty())
        server->ResolveBreakPoints();

      auto bp = server->GetBreakPoint(file_path, line);
      if (bp != nullptr) {
        // Breakpoint hit
        server->frames_ = GetStackFrames();
        server->last_break_file_path_ = file_path;
        server->last_break_line_ = line;
        server->is_stopped_ = true;
        server->ui_->Break(*bp); // Blocked here until ui says continue
        server->ClearBreakData();
      }
    }
  }

//   if (false) {
//     VALUE val;
//     if (EvaluateRubyExpression("local_variables", &val)) {
//       int count = RARRAY_LEN(val);
//       for (int i=0; i < count; ++i) {
//         VALUE var_val = RARRAY_PTR(val)[i];
//         AtLastUstring str = GetRubyObjectAsString(var_val);
//       }
//     }
//   }
}

static int EachKeyValFunc(VALUE key, VALUE val, VALUE data) {
  Server::Impl* impl = reinterpret_cast<Server::Impl*>(data);
  std::string file_path = StringValueCStr(key);

  // See if we added this file yet
  auto itf = impl->script_lines_.find(file_path);
  if (itf == impl->script_lines_.end()) {
    // Add the source lines vector
    auto& lines_vec = impl->script_lines_[file_path];
    // Add the source code
    int n = RARRAY_LEN(val);
    lines_vec.reserve(n);
    for (int i = 0; i < n; ++i) {
      VALUE arr_val = RARRAY_PTR(val)[i];
      const char* val_str = StringValueCStr(arr_val);
      lines_vec.push_back(val_str);
    }
  }
  return ST_CONTINUE;
}

void Server::Impl::ReadScriptLinesHash() {
  rb_hash_foreach(script_lines_hash_, (int(*)(...))EachKeyValFunc, (VALUE)this);
}

bool Server::Impl::ResolveBreakPoint(BreakPoint& bp) {
  bool resolved = false;
  for (auto it = script_lines_.cbegin(),
       ite = script_lines_.cend(); it != ite; ++it) {
    const std::string& file_path = it->first;
    if (FindSubstringCaseInsensitive(file_path, bp.file) >= 0) {
      if (bp.line <= it->second.size()) {
        bp.file = file_path;
        resolved = true;
        break;
      }
    }
  }
  return resolved;
}

void Server::Impl::ResolveBreakPoints() {
  // Make sure we have the loaded files
  ReadScriptLinesHash();

  for (auto it = unresolved_breakpoints_.begin();
       it != unresolved_breakpoints_.end(); ) {
    if (ResolveBreakPoint(*it)) {
      AddBreakPoint(*it, true);
      it = unresolved_breakpoints_.erase(it);
    } else {
      ++it;
    }
  }
}

void Server::Impl::AddBreakPoint(BreakPoint& bp, bool is_resolved) {
  if (bp.index == 0)
    bp.index = ++last_breakpoint_index;
  
  if (is_resolved) {
    auto& bp_map = breakpoints_[bp.line];
    bp_map.insert(std::make_pair(bp.file, bp));
  } else {
    unresolved_breakpoints_.push_back(bp);
  }
}

std::vector<StackFrame> Server::Impl::GetStackFrames() {
  std::vector<StackFrame> frames;
  rb_debug_inspector_open(&DebugInspectorFunc, &frames);
  return frames;
}

Server::Server()
  : impl_(new Impl) {
}

Server::~Server(){
}

Server& Server::Instance() {
  static Server ds;
  return ds;
}

void Server::Start(std::unique_ptr<IDebuggerUI> ui) {
  impl_->EnableTracePoint();

  // Let Ruby collect source files and code into this hash
  impl_->script_lines_hash_ = rb_hash_new();
  rb_define_global_const("SCRIPT_LINES__", impl_->script_lines_hash_);

  impl_->ui_ = std::move(ui);
  impl_->ui_->Initialize(this);
  impl_->is_stopped_ = true;
  impl_->ui_->WaitForContinue();
  impl_->ClearBreakData();
}

bool Server::AddBreakPoint(BreakPoint& bp) {
  boost::lock_guard<boost::mutex> lock(impl_->break_point_mutex_);
  
  // Make sure we have the loaded files
  impl_->ReadScriptLinesHash();

  // Find a matching full file path for the given file.
  bool file_resolved = impl_->ResolveBreakPoint(bp);
  impl_->AddBreakPoint(bp, file_resolved);
  return true;
}

bool Server::RemoveBreakPoint(size_t index) {
  bool removed = false;
  
  // Check resolved breakpoints
  for (auto it = impl_->breakpoints_.begin(), ite = impl_->breakpoints_.end();
       it != ite; ++it) {
    auto& map = it->second;
    for (auto itm = map.begin(), itme = map.end(); itm != itme; ++itm) {
      if (itm->second.index == index) {
        map.erase(itm);
        if (map.empty())
          impl_->breakpoints_.erase(it);
        removed = true;
        break;
      }
    }
    if (removed)
      break;
  }

  // Check unresolved breakpoints
  if (!removed) {
    for (auto it = impl_->unresolved_breakpoints_.begin(),
         ite = impl_->unresolved_breakpoints_.end(); it != ite; ++it) {
      if (index == it->index) {
        impl_->unresolved_breakpoints_.erase(it);
        removed = true;
        break;
      }
    }
  }

  return removed;
}

std::vector<BreakPoint> Server::GetBreakPoints() const {
  // Try to resolve any unresolved breakpoints
  impl_->ResolveBreakPoints();

  std::vector<BreakPoint> bps;
  
  // Add resolved breakpoints
  for (auto it = impl_->breakpoints_.cbegin(), ite = impl_->breakpoints_.cend();
       it != ite; ++it) {
    auto& map = it->second;
    for (auto itm = map.cbegin(), itme = map.cend(); itm != itme; ++itm) {
      bps.push_back(itm->second);
    }
  }

  // Add unresolved breakpoints
  std::copy(impl_->unresolved_breakpoints_.cbegin(),
            impl_->unresolved_breakpoints_.cend(), std::back_inserter(bps));

  // Sort by index
  std::sort(bps.begin(), bps.end(), &SortBreakPoints);
  return bps;
}

bool Server::IsStopped() const {
  return impl_->is_stopped_;
}

std::string Server::EvaluateExpression(const std::string& expr) {
 std::string eval_res;
 if (!impl_->frames_.empty() &&
     impl_->active_frame_index_ < impl_->frames_.size()) {
   const auto& cur_frame = impl_->frames_[impl_->active_frame_index_];
   eval_res = EvaluateRubyExpression(expr, cur_frame.binding);
 } else {
   eval_res = "Expression cannot be evaluated";
 }
 return eval_res;
}

std::vector<StackFrame> Server::GetStackFrames() const {
  return impl_->frames_;
}

void Server::ShiftActiveFrame(bool shift_up) {
  if (IsStopped()) {
    if (shift_up) {
      if (impl_->active_frame_index_ + 1 < impl_->frames_.size())
        impl_->active_frame_index_ += 1;
    } else {
      if (impl_->active_frame_index_ > 0)
        impl_->active_frame_index_ -= 1;
    }
  }
}

size_t Server::GetActiveFrameIndex() const {
  return impl_->active_frame_index_;
}

void Server::Step() {
  if (IsStopped())
    impl_->break_at_next_line_ = true;
}

std::vector<std::pair<size_t, std::string>>
      Server::GetCodeLines(size_t beg_line, size_t end_line) const {
  std::vector<std::pair<size_t, std::string>> lines;
  if (IsStopped()) {
    impl_->ReadScriptLinesHash();

    auto itf = impl_->script_lines_.find(impl_->last_break_file_path_);
    if (itf != impl_->script_lines_.end()) {
      const auto& lines_vec = itf->second;
      const size_t expand_lines = 5;
      if (beg_line == 0) {
        beg_line = impl_->last_break_line_;
        if (beg_line > expand_lines)
          beg_line -= expand_lines;
        else
          beg_line = 1;
      }
      if (end_line == 0) {
        end_line = impl_->last_break_line_ + expand_lines;
      }
      if (end_line >= lines_vec.size() + 1)
        end_line = lines_vec.size();
      if (end_line >= beg_line) {
        for (size_t i = beg_line - 1; i < end_line; ++i) {
          lines.push_back(std::make_pair(i+1, lines_vec[i]));
        }
      }
    }
  }
  return lines;
}

size_t Server::GetBreakLineNumber() const {
  return impl_->last_break_line_;
}

} // end namespace RubyDebugger
} // end namespace SketchUp