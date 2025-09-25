#pragma once
#include "DataManager.hpp"

#include <sys/queue.h>
#include <event.h>
// for http
#include <evhttp.h>
#include <event2/http.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <regex>

#include "base64.h" // 来自 cpp-base64 库

extern storage::DataManager *data_;
namespace storage
{
    class Service
    {
    public:
        Service() {
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Debug("Service start(Construct)");
#endif
            server_port_ = Config::GetInstance()->GetServerPort();
            server_ip_ = Config::GetInstance()->GetServerIp();
            download_prefix_ = Config::GetInstance()->GetDownloadPrefix();
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Debug("Service end(Construct)");
#endif
        }
        bool RunModule() {
            // 初始化环境
            event_base *base = event_base_new();
            if (base == NULL)
            {
                mylog::GetLogger("asynclogger")->Fatal("event_base_new err!");
                return false;
            }
            // 设置监听的端口和地址
            sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(server_port_);
            // http 服务器,创建evhttp上下文
            evhttp *httpd = evhttp_new(base);
            // 绑定端口和ip
            if (evhttp_bind_socket(httpd, "0.0.0.0", server_port_) != 0)
            {
                mylog::GetLogger("asynclogger")->Fatal("evhttp_bind_socket failed!");
                return false;
            }
            // 设定回调函数
            // 指定generic callback，也可以为特定的URI指定callback
            evhttp_set_gencb(httpd, GenHandler, NULL);

            if (base)
            {
#ifdef DEBUG_LOG
                mylog::GetLogger("asynclogger")->Debug("event_base_dispatch");
#endif
                if (-1 == event_base_dispatch(base))
                {
                    mylog::GetLogger("asynclogger")->Debug("event_base_dispatch err");
                }
            }
            if (base)
                event_base_free(base);
            if (httpd)
                evhttp_free(httpd);
            return true;
        }

    private:
        uint16_t server_port_;
        std::string server_ip_;
        std::string download_prefix_;

    private:
        static void GenHandler(struct evhttp_request *req, void *arg) {
            std::string path = evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req));
            path = UrlDecode(path);
            mylog::GetLogger("asynclogger")->Info("get req, uri: %s", path.c_str());

            // 根据请求中的内容判断是什么请求
            // 这里是下载请求
            if (path.find("/download/") != std::string::npos)
            {
                Download(req, arg);
            }
            // 这里是上传
            else if (path == "/upload")
            {
                Upload(req, arg);
            }
            // 这里是删除
            else if (path == "/delete")
            {
                Delete(req, arg);
            }
            // 这里就是显示已存储文件列表，返回一个html页面给浏览器
            else if (path == "/")
            {
                ListShow(req, arg);
            }
            else
            {
                evhttp_send_reply(req, HTTP_NOTFOUND, "Not Found", NULL);
            }
        }

        static void Upload(struct evhttp_request *req, void *arg) {
            mylog::GetLogger("asynclogger")->Info("Upload start");
            // 约定：请求中包含"low_storage"，说明请求中存在文件数据,并希望普通存储\
                包含"deep_storage"字段则压缩后存储
            // 获取请求体内容
            struct evbuffer *buf = evhttp_request_get_input_buffer(req);
            if (buf == nullptr)
            {
                mylog::GetLogger("asynclogger")->Info("evhttp_request_get_input_buffer is empty");
                return;
            }

            size_t len = evbuffer_get_length(buf); // 获取请求体的长度
            mylog::GetLogger("asynclogger")->Info("evbuffer_get_length is %u", len);
            if (0 == len)
            {
                evhttp_send_reply(req, HTTP_BADREQUEST, "file empty", NULL);
                mylog::GetLogger("asynclogger")->Info("request body is empty");
                return;
            }
            std::string content(len, 0);
            if (-1 == evbuffer_copyout(buf, (void *)content.c_str(), len))
            {
                mylog::GetLogger("asynclogger")->Error("evbuffer_copyout error");
                evhttp_send_reply(req, HTTP_INTERNAL, NULL, NULL);
                return;
            }

            // 获取文件名
            std::string filename = evhttp_find_header(req->input_headers, "FileName");
            // 解码文件名
            filename = base64_decode(filename);

            // 获取存储类型，客户端自定义请求头 StorageType
            std::string storage_type = evhttp_find_header(req->input_headers, "StorageType");
            // 组织存储路径
            std::string storage_path;
            if (storage_type == "low")
            {
                storage_path = Config::GetInstance()->GetLowStorageDir();
            }
            else if (storage_type == "deep")
            {
                storage_path = Config::GetInstance()->GetDeepStorageDir();
            }
            else
            {
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: HTTP_BADREQUEST");
                evhttp_send_reply(req, HTTP_BADREQUEST, "Illegal storage type", NULL);
                return;
            }

            // 如果不存在就创建low或deep目录
            FileUtil dirCreate(storage_path);
            dirCreate.CreateDirectory();

            // 目录创建后加可以加上文件名，这个就是最终要写入的文件路径
            storage_path += filename;
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Debug("storage_path:%s", storage_path.c_str());
#endif

            // 看路径里是low还是deep存储，是deep就压缩，是low就直接写入
            FileUtil fu(storage_path);
            if (storage_path.find("low_storage") != std::string::npos)
            {
                if (fu.SetContent(content.c_str(), len) == false)
                {
                    mylog::GetLogger("asynclogger")->Error("low_storage fail, evhttp_send_reply: HTTP_INTERNAL");
                    evhttp_send_reply(req, HTTP_INTERNAL, "server error", NULL);
                    return;
                }
                else
                {
                    mylog::GetLogger("asynclogger")->Info("low_storage success");
                }
            }
            else
            {
                if (fu.Compress(content, Config::GetInstance()->GetBundleFormat()) == false)
                {
                    mylog::GetLogger("asynclogger")->Error("deep_storage fail, evhttp_send_reply: HTTP_INTERNAL");
                    evhttp_send_reply(req, HTTP_INTERNAL, "server error", NULL);
                    return;
                }
                else
                {
                    mylog::GetLogger("asynclogger")->Info("deep_storage success");
                }
            }

            // 添加存储文件信息，交由数据管理类进行管理
            StorageInfo info;
            info.NewStorageInfo(storage_path); // 组织存储的文件信息
            data_->Insert(info);               // 向数据管理模块添加存储的文件信息

            evhttp_send_reply(req, HTTP_OK, "Success", NULL);
            mylog::GetLogger("asynclogger")->Info("upload finish:success");
        }

        static std::string TimetoStr(time_t t) {
            struct tm timeinfo;
            localtime_r(&t, &timeinfo);
            
            char buffer[80];
            strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
            return std::string(buffer);
        }

        // 前端代码处理函数
        // 在渲染函数中直接处理StorageInfo
        static std::string generateModernFileList(const std::vector<StorageInfo> &files) {
            std::stringstream ss;
            
            // 如果没有文件，显示空状态
            if (files.empty()) {
                ss << "<div class=\"empty-state\">";
                ss << "<i class=\"fas fa-folder-open\"></i>";
                ss << "<h3>暂无文件</h3>";
                ss << "<p>上传文件后将显示在此处</p>";
                ss << "</div>";
                return ss.str();
            }

            // 有文件时，生成文件列表
            for (const auto &file : files)
            {
                std::string filename = FileUtil(file.storage_path_).FileName();

                // 从路径中解析存储类型
                std::string storage_type = "low";
                if (file.storage_path_.find("deep") != std::string::npos)
                {
                    storage_type = "deep";
                }

                // 根据文件扩展名选择合适的图标
                std::string file_icon = "fa-file";
                size_t dot_pos = filename.find_last_of(".");
                if (dot_pos != std::string::npos) {
                    std::string ext = filename.substr(dot_pos + 1);
                    if (ext == "pdf") file_icon = "fa-file-pdf";
                    else if (ext == "doc" || ext == "docx") file_icon = "fa-file-word";
                    else if (ext == "xls" || ext == "xlsx") file_icon = "fa-file-excel";
                    else if (ext == "ppt" || ext == "pptx") file_icon = "fa-file-powerpoint";
                    else if (ext == "zip" || ext == "rar" || ext == "7z" || ext == "tar" || ext == "gz") file_icon = "fa-file-archive";
                    else if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "bmp" || ext == "svg") file_icon = "fa-file-image";
                    else if (ext == "mp3" || ext == "wav" || ext == "ogg" || ext == "flac") file_icon = "fa-file-audio";
                    else if (ext == "mp4" || ext == "avi" || ext == "mov" || ext == "wmv" || ext == "mkv") file_icon = "fa-file-video";
                    else if (ext == "txt" || ext == "log" || ext == "md") file_icon = "fa-file-alt";
                    else if (ext == "html" || ext == "htm" || ext == "xml" || ext == "json" || ext == "js" || ext == "css") file_icon = "fa-file-code";
                }

                // 格式化时间
                std::string time_str = TimetoStr(file.mtime_);
                // 移除末尾的换行符
                if (!time_str.empty() && time_str[time_str.length() - 1] == '\n') {
                    time_str = time_str.substr(0, time_str.length() - 1);
                }

                ss << "<div class=\"file-item\">";
                
                // 文件图标
                ss << "<div class=\"file-icon\"><i class=\"fas " << file_icon << "\"></i></div>";
                
                // 文件信息
                ss << "<div class=\"file-info\">";
                ss << "<div class=\"file-name\">" << filename << "</div>";
                ss << "<div class=\"file-details\">";
                ss << "<span><i class=\"fas fa-hdd\"></i> " << (storage_type == "deep" ? "深度存储" : "普通存储") << "</span>";
                ss << "<span><i class=\"fas fa-weight-hanging\"></i> " << formatSize(file.fsize_) << "</span>";
                ss << "<span><i class=\"fas fa-clock\"></i> " << time_str << "</span>";
                ss << "</div>";
                ss << "</div>";
                
                // 文件操作
                ss << "<div class=\"file-actions\">";
                ss << "<button class=\"action-btn btn-success\" onclick=\"downloadFile('" << file.url_ << "')\"><i class=\"fas fa-download\"></i> 下载</button>";
                ss << "<button class=\"action-btn btn-danger\" onclick=\"deleteFile('" << file.url_ << "', '" << filename << "')\"><i class=\"fas fa-trash-alt\"></i> 删除</button>";
                ss << "</div>";
                
                ss << "</div>";
            }

            return ss.str();
        }

        // 文件大小格式化函数
        static std::string formatSize(uint64_t bytes) {
            const char *units[] = {"B", "KB", "MB", "GB"};
            int unit_index = 0;
            double size = bytes;

            while (size >= 1024 && unit_index < 3)
            {
                size /= 1024;
                unit_index++;
            }

            std::stringstream ss;
            ss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
            return ss.str();
        }
        static void ListShow(struct evhttp_request *req, void *arg) {
            mylog::GetLogger("asynclogger")->Info("ListShow()");
            // 1. 获取所有的文件存储信息
            std::vector<StorageInfo> arry;
            data_->GetAll(&arry);

            // 读取模板文件
            std::ifstream templateFile("index.html");
            std::string templateContent(
                (std::istreambuf_iterator<char>(templateFile)),
                std::istreambuf_iterator<char>());

            // 替换html文件中的占位符
            //替换文件列表进html
            templateContent = std::regex_replace(templateContent,
                                                 std::regex("\\{\\{FILE_LIST\\}\\}"),
                                                 generateModernFileList(arry));
            //替换服务器地址进hrml
            templateContent = std::regex_replace(templateContent,
                                                 std::regex("\\{\\{BACKEND_URL\\}\\}"),
                                                "http://"+storage::Config::GetInstance()->GetServerIp()+":"+std::to_string(storage::Config::GetInstance()->GetServerPort()));
            // 获取请求的输出evbuffer
            struct evbuffer *buf = evhttp_request_get_output_buffer(req);
            auto response_body = templateContent;
            // 把前面的html数据给到evbuffer，然后设置响应头部字段，最后返回给浏览器
            evbuffer_add(buf, (const void *)response_body.c_str(), response_body.size());
            evhttp_add_header(req->output_headers, "Content-Type", "text/html;charset=utf-8");
            evhttp_send_reply(req, HTTP_OK, NULL, NULL);
            mylog::GetLogger("asynclogger")->Info("ListShow() finish");
        }
        static std::string GetETag(const StorageInfo &info) {
            // 自定义etag :  filename-fsize-mtime
            FileUtil fu(info.storage_path_);
            std::string etag = fu.FileName();
            etag += "-";
            etag += std::to_string(info.fsize_);
            etag += "-";
            etag += std::to_string(info.mtime_);
            return etag;
        }
        static void Download(struct evhttp_request *req, void *arg) {
            // 1. 获取客户端请求的资源路径path   req.path
            // 2. 根据资源路径，获取StorageInfo
            StorageInfo info;
            std::string resource_path = evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req));
            resource_path = UrlDecode(resource_path);
            data_->GetOneByURL(resource_path, &info);
            mylog::GetLogger("asynclogger")->Info("request resource_path:%s", resource_path.c_str());

            std::string download_path = info.storage_path_;
            // 2.如果压缩过了就解压到新文件给用户下载
            if (info.storage_path_.find(Config::GetInstance()->GetLowStorageDir()) == std::string::npos)
            {
                mylog::GetLogger("asynclogger")->Info("uncompressing:%s", info.storage_path_.c_str());
                FileUtil fu(info.storage_path_);
                download_path = Config::GetInstance()->GetLowStorageDir() +
                                std::string(download_path.begin() + download_path.find_last_of('/') + 1, download_path.end());
                FileUtil dirCreate(Config::GetInstance()->GetLowStorageDir());
                dirCreate.CreateDirectory();
                fu.UnCompress(download_path); // 将文件解压到low_storage下去或者再创一个文件夹做中转
            }
            mylog::GetLogger("asynclogger")->Info("request download_path:%s", download_path.c_str());
            FileUtil fu(download_path);
            if (fu.Exists() == false && info.storage_path_.find("deep_storage") != std::string::npos)
            {
                // 如果是压缩文件，且解压失败，是服务端的错误
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: 500 - UnCompress failed");
                evhttp_send_reply(req, HTTP_INTERNAL, NULL, NULL);
            }
            else if (fu.Exists() == false && info.storage_path_.find("low_storage") == std::string::npos)
            {
                // 如果是普通文件，且文件不存在，是客户端的错误
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: 400 - bad request,file not exists");
                evhttp_send_reply(req, HTTP_BADREQUEST, "file not exists", NULL);
            }

            // 3.确认文件是否需要断点续传
            bool retrans = false;
            std::string old_etag;
            auto if_range = evhttp_find_header(req->input_headers, "If-Range");
            if (NULL != if_range)
            {
                old_etag = if_range;
                // 有If-Range字段且，这个字段的值与请求文件的最新etag一致则符合断点续传
                if (old_etag == GetETag(info))
                {
                    retrans = true;
                    mylog::GetLogger("asynclogger")->Info("%s need breakpoint continuous transmission", download_path.c_str());
                }
            }

            // 4. 读取文件数据，放入rsp.body中
            if (fu.Exists() == false)
            {
                mylog::GetLogger("asynclogger")->Info("%s not exists", download_path.c_str());
                download_path += "not exists";
                evhttp_send_reply(req, 404, download_path.c_str(), NULL);
                return;
            }
            evbuffer *outbuf = evhttp_request_get_output_buffer(req);
            int fd = open(download_path.c_str(), O_RDONLY);
            if (fd == -1)
            {
                mylog::GetLogger("asynclogger")->Error("open file error: %s -- %s", download_path.c_str(), strerror(errno));
                evhttp_send_reply(req, HTTP_INTERNAL, strerror(errno), NULL);
                return;
            }
            // 和前面用的evbuffer_add类似，但是效率更高，具体原因可以看函数声明
            if (-1 == evbuffer_add_file(outbuf, fd, 0, fu.FileSize()))
            {
                mylog::GetLogger("asynclogger")->Error("evbuffer_add_file: %d -- %s -- %s", fd, download_path.c_str(), strerror(errno));
            }
            // 5. 设置响应头部字段： ETag， Accept-Ranges: bytes
            evhttp_add_header(req->output_headers, "Accept-Ranges", "bytes");
            evhttp_add_header(req->output_headers, "ETag", GetETag(info).c_str());
            evhttp_add_header(req->output_headers, "Content-Type", "application/octet-stream");
            if (retrans == false)
            {
                evhttp_send_reply(req, HTTP_OK, "Success", NULL);
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: HTTP_OK");
            }
            else
            {
                evhttp_send_reply(req, 206, "breakpoint continuous transmission", NULL); // 区间请求响应的是206
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: 206");
            }
            if (download_path != info.storage_path_)
            {
                remove(download_path.c_str()); // 删除文件
            }
        }

        static void Delete(struct evhttp_request *req, void *arg) {
            mylog::GetLogger("asynclogger")->Info("Delete start");
            
            // 获取请求方法，确保是POST请求
            if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
                mylog::GetLogger("asynclogger")->Warn("Delete: Not a POST request");
                evhttp_send_reply(req, HTTP_BADREQUEST, "Bad Request: Method not allowed", NULL);
                return;
            }
            
            // 获取要删除的文件URL
            struct evkeyvalq params;
            const char* uri = evhttp_request_get_uri(req);
            evhttp_parse_query(uri, &params);
            const char* file_url = evhttp_find_header(&params, "url");
            
            if (file_url == NULL) {
                mylog::GetLogger("asynclogger")->Warn("Delete: Missing file URL");
                evhttp_send_reply(req, HTTP_BADREQUEST, "Bad Request: Missing file URL", NULL);
                evhttp_clear_headers(&params);
                return;
            }
            
            std::string url = file_url;
            mylog::GetLogger("asynclogger")->Info("Deleting file with URL: %s", url.c_str());
            
            // 调用DataManager删除文件
            bool success = data_->DeleteByURL(url);
            evhttp_clear_headers(&params);
            
            // 准备响应
            struct evbuffer *buf = evhttp_request_get_output_buffer(req);
            if (success) {
                // 成功响应
                evbuffer_add_printf(buf, "{\"status\": \"success\", \"message\": \"文件删除成功\"}");
                evhttp_add_header(req->output_headers, "Content-Type", "application/json;charset=utf-8");
                evhttp_send_reply(req, HTTP_OK, "Success", NULL);
                mylog::GetLogger("asynclogger")->Info("Delete: Success");
            } else {
                // 失败响应
                evbuffer_add_printf(buf, "{\"status\": \"error\", \"message\": \"文件删除失败\"}");
                evhttp_add_header(req->output_headers, "Content-Type", "application/json;charset=utf-8");
                evhttp_send_reply(req, HTTP_INTERNAL, "Server Error", NULL);
                mylog::GetLogger("asynclogger")->Error("Delete: Failed");
            }
        }
    };
}
