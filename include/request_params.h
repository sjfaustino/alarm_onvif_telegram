#pragma once
#include <PsychicHttp.h>
#include "webserver_html.h" // FormParams

// FormParams over a live dashboard request.
class RequestParams : public FormParams {
 public:
  explicit RequestParams(PsychicRequest* request) : request_(request) {}
  bool has(const char* name) const override { return request_->hasParam(name); }
  String get(const char* name, const char* fallback) const override { return request_->getParam(name, fallback); }

 private:
  PsychicRequest* request_;
};
