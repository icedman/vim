extern "C" {

#include <lua.h>

#include <tree_sitter/api.h>
const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_cpp(void);
const TSLanguage *tree_sitter_c_sharp(void);
const TSLanguage *tree_sitter_css(void);
const TSLanguage *tree_sitter_html(void);
const TSLanguage *tree_sitter_java(void);
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_json(void);
const TSLanguage *tree_sitter_python(void);
}

#define EXPORT                                                                 \
  extern "C" __attribute__((visibility("default"))) __attribute__((used))

#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

extern "C" {
typedef struct file_buffer buf_T;
typedef struct buf_snapshot buf_snapshot_T;
typedef long linenr_T;
typedef int colnr_T;

extern buf_T *curbuf;
buf_snapshot_T *buf_snapshot_acquire(buf_T *buf);
int buf_snapshot_threadsafe(void);
void buf_snapshot_unref(buf_snapshot_T *snapshot);
linenr_T buf_snapshot_line_count(const buf_snapshot_T *snapshot);
const unsigned char *buf_snapshot_line(const buf_snapshot_T *snapshot,
                                       linenr_T lnum);
colnr_T buf_snapshot_line_len(const buf_snapshot_T *snapshot, linenr_T lnum);
}

#define LOG_FILE "/tmp/vim-treesitter.log"

static bool log_initialized = false;
static bool log_tree = false;

void initLog() {
  FILE *log_file = fopen(LOG_FILE, "w");
  if (log_file) {
    fclose(log_file);
  }
  log_initialized = true;
}

void log(const char *format, ...) {
  if (!log_initialized) {
    initLog();
  }

  static char string[1024] = "";

  va_list args;
  va_start(args, format);
  vsnprintf(string, 1024, format, args);
  va_end(args);

  FILE *log_file = fopen(LOG_FILE, "a");
  if (!log_file) {
    return;
  }
  char *token = strtok(string, "\n");
  while (token != NULL) {
    fprintf(log_file, "%s", token);
    fprintf(log_file, "\n");
    token = strtok(NULL, "\n");
  }
  fclose(log_file);
}

std::map<std::string, std::function<const TSLanguage *()>> ts_languages = {
    {".c", tree_sitter_c},
    {".cpp", tree_sitter_cpp},
    {".hpp", tree_sitter_cpp},
    {".h", tree_sitter_cpp},
    {".cs", tree_sitter_c_sharp},
    {".css", tree_sitter_css},
    {".html", tree_sitter_html},
    {".xml", tree_sitter_html},
    {".java", tree_sitter_java},
    {".js", tree_sitter_javascript},
    {".jsx", tree_sitter_javascript},
    {".json", tree_sitter_json},
    {".py", tree_sitter_python},
};

class TSNodeEx {
public:
  TSNodeEx(TSNode n, int d, char *t = NULL) {
    node = n;
    depth = d;
    if (t) {
      context = t;
    }
  }
  TSNode node;
  int depth;
  std::string context;
};

class Doc {
public:
  Doc() : tree(NULL), generation(0), published_generation(0) {}
  ~Doc() {
    if (tree) {
      ts_tree_delete(tree);
    }
  }
  std::mutex mutex;
  TSTree *tree;
  unsigned long generation;
  unsigned long published_generation;
};

typedef std::shared_ptr<Doc> doc_ptr;
static std::map<int, doc_ptr> docs;
static std::mutex docs_mutex;

doc_ptr get_doc(int id) {
  std::lock_guard<std::mutex> lock(docs_mutex);
  if (docs.find(id) == docs.end()) {
    docs[id] = std::make_shared<Doc>();
  }
  return docs[id];
}

TSTree *build_tree(char *content, int content_length, char *type);

struct ParseJob {
  int id;
  unsigned long generation;
  buf_snapshot_T *snapshot;
  std::string type;
};

class ParseWorker {
public:
  ParseWorker() : stopping(false), worker(&ParseWorker::run, this) {}

  ~ParseWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    changed.notify_one();
    worker.join();
    for (auto &entry : pending) {
      buf_snapshot_unref(entry.second.snapshot);
    }
  }

  void submit(ParseJob job) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto old = pending.find(job.id);
      if (old != pending.end()) {
        buf_snapshot_unref(old->second.snapshot);
        old->second = std::move(job);
      } else {
        pending.emplace(job.id, std::move(job));
      }
    }
    changed.notify_one();
  }

private:
  void run() {
    for (;;) {
      ParseJob job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [this] { return stopping || !pending.empty(); });
        if (stopping) {
          return;
        }
        auto next = pending.begin();
        job = std::move(next->second);
        pending.erase(next);
      }

      std::string content;
      linenr_T line_count = buf_snapshot_line_count(job.snapshot);
      for (linenr_T lnum = 1; lnum <= line_count; ++lnum) {
        const unsigned char *line = buf_snapshot_line(job.snapshot, lnum);
        colnr_T length = buf_snapshot_line_len(job.snapshot, lnum);
        if (line != NULL) {
          content.append(reinterpret_cast<const char *>(line), length);
        }
        content.push_back('\n');
      }
      buf_snapshot_unref(job.snapshot);

      TSTree *tree = build_tree(content.data(), static_cast<int>(content.size()),
                                const_cast<char *>(job.type.c_str()));
      doc_ptr doc = get_doc(job.id);
      std::lock_guard<std::mutex> lock(doc->mutex);
      if (doc->generation == job.generation) {
        if (doc->tree) {
          ts_tree_delete(doc->tree);
        }
        doc->tree = tree;
        doc->published_generation = job.generation;
      } else if (tree) {
        ts_tree_delete(tree);
      }
    }
  }

  std::mutex mutex;
  std::condition_variable changed;
  std::map<int, ParseJob> pending;
  bool stopping;
  std::thread worker;
};

static ParseWorker &parse_worker() {
  static ParseWorker instance;
  return instance;
}

void walk_tree(TSTreeCursor *cursor, int depth, int row,
               std::vector<TSNodeEx> *nodes, int option = 0,
               char *last_type = NULL) {
  TSNode node = ts_tree_cursor_current_node(cursor);


  const char *type = ts_node_type(node);
  TSPoint startPoint = ts_node_start_point(node);
  TSPoint endPoint = ts_node_end_point(node);

  // if (strcmp(type, "ERROR") == 0) {
  // nodes->clear();
  // return;
  // }

  bool has_children = true;
  if (!ts_tree_cursor_goto_first_child(cursor)) {
    has_children = false;
  }

  if (nodes != NULL) {
    // Find a specific row, or every highlightable node when row is -1.
    if (option == 0 && (!has_children || startPoint.row != endPoint.row)) {
      if (row == -1 || startPoint.row == static_cast<uint32_t>(row)) {
        nodes->push_back(TSNodeEx(node, depth, last_type));
      }
    }

    // find block section ~ option 1
    if (row != -1 && option == 1 &&
        (has_children && startPoint.row != endPoint.row)) {
      if (startPoint.row < static_cast<uint32_t>(row) &&
          endPoint.row > static_cast<uint32_t>(row)) {
        nodes->push_back(TSNodeEx(node, depth, last_type));
      }
    }
  }

  if (!has_children) {
    return;
  }

  do {
    TSTreeCursor child_cursor =
        ts_tree_cursor_new(ts_tree_cursor_current_node(cursor));
    walk_tree(&child_cursor, depth + 1, row, nodes, option, (char *)type);
    ts_tree_cursor_delete(&child_cursor);
  } while (ts_tree_cursor_goto_next_sibling(cursor));
}

TSTree *build_tree(char *content, int content_length, char *type) {
  if (!type || ts_languages.find(type) == ts_languages.end()) {
    return NULL;
  }

  std::string langId = type;
  std::function<const TSLanguage *()> lang = ts_languages[langId];

  TSParser *parser = ts_parser_new();
  if (!ts_parser_set_language(parser, lang())) {
    log("invalid language\n");
    return NULL;
  } else {
    // log("language: %s", type);
  }

  TSTree *tree = ts_parser_parse_string(parser, NULL /* TODO: old_tree */,
                                        content, content_length);

  ts_parser_delete(parser);
  return tree;
}

void dump_nodes(std::vector<TSNodeEx> nodes) {
  for (auto _node : nodes) {
    TSNode node = _node.node;
    const char *type = ts_node_type(node);
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);

    std::stringstream ss;
    for (int i = 0; i < _node.depth; i++) {
      ss << " ";
    }
    ss << start.row;
    ss << ",";
    ss << start.column;
    ss << "-";
    ss << end.row;
    ss << ",";
    ss << end.column;
    ss << " ";
    ss << type;

    log(">%s\n", ss.str().c_str());
  }
}

std::vector<TSNodeEx> query_tree(TSTree *tree, int row, int option = 0) {
  std::vector<TSNodeEx> nodes;

  if (tree) {
    TSNode root_node = ts_tree_root_node(tree);

    TSTreeCursor cursor = ts_tree_cursor_new(root_node);
    walk_tree(&cursor, 0, row, &nodes, option);
    ts_tree_cursor_delete(&cursor);
  }

  if (log_tree) {
    dump_nodes(nodes);
  }
  return nodes;
}



int _query_tree(lua_State *L) {
  int id = lua_tonumber(L, -3);
  int row = lua_tonumber(L, -2);
  int option = lua_tonumber(L, -1);

  lua_newtable(L);
  int idx = 1;

  doc_ptr doc = get_doc(id);
  std::lock_guard<std::mutex> lock(doc->mutex);
  if (!doc->tree) {
    return 1;
  }

  if (doc->tree) {
    std::vector<TSNodeEx> nodes = query_tree(doc->tree, row, option);
    for (auto _node : nodes) {

      TSNode node = _node.node;
      const char *type = ts_node_type(node);
      TSPoint start = ts_node_start_point(node);
      TSPoint end = ts_node_end_point(node);

      std::string _t = _node.context;
      if (_t.size() > 0) {
        _t = _t + ".";
      }
      _t = _t + type;

      int col = 1;
      lua_newtable(L);
      lua_pushnumber(L, start.row);
      lua_rawseti(L, -2, col++);
      lua_pushnumber(L, start.column);
      lua_rawseti(L, -2, col++);
      lua_pushnumber(L, end.row);
      lua_rawseti(L, -2, col++);
      lua_pushnumber(L, end.column);
      lua_rawseti(L, -2, col++);
      lua_pushstring(L, _t.c_str());
      lua_rawseti(L, -2, col++);

      lua_rawseti(L, -2, idx++);
    }
  }

  return 1;
}

int _parse_buffer(lua_State *L) {
  const char *type = lua_tostring(L, -2);
  int id = lua_tonumber(L, -1);

  if (type == NULL || ts_languages.find(type) == ts_languages.end() ||
      !buf_snapshot_threadsafe()) {
    lua_pushboolean(L, 0);
    return 1;
  }

  buf_snapshot_T *snapshot = buf_snapshot_acquire(curbuf);
  if (snapshot == NULL) {
    lua_pushboolean(L, 0);
    return 1;
  }

  doc_ptr doc = get_doc(id);
  unsigned long generation;
  {
    std::lock_guard<std::mutex> lock(doc->mutex);
    generation = ++doc->generation;
  }
  parse_worker().submit({id, generation, snapshot, type});
  lua_pushnumber(L, generation);
  return 1;
}

int _parse_ready(lua_State *L) {
  int id = lua_tonumber(L, -2);
  unsigned long generation = lua_tonumber(L, -1);
  doc_ptr doc = get_doc(id);
  std::lock_guard<std::mutex> lock(doc->mutex);
  lua_pushboolean(L, doc->published_generation >= generation);
  return 1;
}

int _log_tree(lua_State *L) {
  int id = lua_tonumber(L, -1);
  doc_ptr doc = get_doc(id);
  std::lock_guard<std::mutex> lock(doc->mutex);
  if (!doc->tree) {
    lua_pushboolean(L, 0);
    return 1;
  }

  char *tree_string = ts_node_string(ts_tree_root_node(doc->tree));
  FILE *log_file = fopen(LOG_FILE, "w");
  if (tree_string == NULL || log_file == NULL) {
    free(tree_string);
    if (log_file != NULL) {
      fclose(log_file);
    }
    lua_pushboolean(L, 0);
    return 1;
  }
  fputs(tree_string, log_file);
  fputc('\n', log_file);
  fclose(log_file);
  free(tree_string);
  lua_pushboolean(L, 1);
  return 1;
}

EXPORT int luaopen_treesitter(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, _parse_buffer);
  lua_setfield(L, -2, "parse_buffer");
  lua_pushcfunction(L, _query_tree);
  lua_setfield(L, -2, "query_tree");
  lua_pushcfunction(L, _parse_ready);
  lua_setfield(L, -2, "parse_ready");
  lua_pushcfunction(L, _log_tree);
  lua_setfield(L, -2, "log_tree");
  return 1;
}

#ifdef HAVE_MAIN
int main(int argc, char **argv) {
  if (argc == 2) {
    std::stringstream ss;
    std::ifstream t(argv[1]);
    ss << t.rdbuf();
    TSTree *tree = build_tree((char *)(ss.str().c_str()), ss.str().size(), ".c");
    query_tree(tree, 76);
    ts_tree_delete(tree);
  }
  return 0;
}

// 76,2-76,11 expression_statement
//  76,2-76,10 call_expression
//   76,2-76,8 identifier
//   76,8-76,10 argument_list
//    76,8-76,9 (
//    76,9-76,10 )
//  76,10-76,11 ;

#endif
