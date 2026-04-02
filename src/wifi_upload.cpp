#include "wifi_upload.h"
#include "config.h"
#include "settings.h"
#include "reader.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <ArduinoJson.h>

static WebServer _server(80);
static bool _running = false;
static File _uploadFile;
static BookReader* _reader = nullptr;

void wifi_upload_set_reader(BookReader* reader) { _reader = reader; }

static const char UPLOAD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>E-Reader Upload</title>
<style>
  body { font-family: sans-serif; max-width: 600px; margin: 40px auto; padding: 0 20px; background: #f5f5f5; }
  h1 { color: #333; }
  .upload-form { background: #fff; padding: 30px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
  input[type="file"] { margin: 15px 0; }
  button { background: #333; color: #fff; border: none; padding: 12px 24px; border-radius: 4px; cursor: pointer; font-size: 16px; }
  button:hover { background: #555; }
  .status { margin-top: 20px; padding: 10px; border-radius: 4px; }
  .success { background: #d4edda; color: #155724; }
  .error { background: #f8d7da; color: #721c24; }
  .books { margin-top: 20px; }
  .books li { padding: 4px 0; display: flex; justify-content: space-between; align-items: center; }
  .del-btn { background: #c00; color: #fff; border: none; padding: 4px 10px; border-radius: 4px; cursor: pointer; font-size: 12px; }
  .del-btn:hover { background: #e00; }
  .settings-form { background: #fff; padding: 30px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-top: 20px; }
  .settings-form label { display: block; margin-top: 10px; font-weight: bold; }
  .settings-form input[type="text"], .settings-form input[type="password"] { width: 100%; padding: 8px; margin-top: 4px; box-sizing: border-box; border: 1px solid #ccc; border-radius: 4px; }
</style>
</head>
<body>
<h1>E-Reader Upload</h1>
<div class="upload-form">
  <p>Upload EPUB files to <code>/books</code> and PNG/JPG sleep images to <code>/sleep</code>.</p>
  <form method="POST" action="/upload" enctype="multipart/form-data">
    <input type="file" name="file" accept=".epub,.png,.jpg,.jpeg"><br>
    <button type="submit">Upload</button>
  </form>
</div>
<div class="books">
  <h2>Books on device</h2>
  <ul id="booklist"></ul>
</div>
<div class="books">
  <h2>Sleep images</h2>
  <ul id="imagelist"></ul>
</div>
<div class="settings-form">
  <h2>WiFi Settings</h2>
  <label>SSID</label>
  <input type="text" id="wifiSSID">
  <label>Password</label>
  <input type="password" id="wifiPass">
  <br><br>
  <button onclick="saveSettings()">Save WiFi Settings</button>
  <div id="settingsStatus" class="status" style="display:none"></div>
</div>
<script>
fetch('/list').then(r=>r.json()).then(d=>{
  const ul=document.getElementById('booklist');
  d.books.forEach(f=>{
    const li=document.createElement('li');
    const span=document.createElement('span');span.textContent=f;
    const btn=document.createElement('button');btn.textContent='Delete';btn.className='del-btn';
    btn.onclick=function(){if(confirm('Delete '+f+'?')){
      fetch('/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({filename:f})})
      .then(r=>r.json()).then(r=>{if(r.ok){li.remove();}else{alert(r.error||'Delete failed');}});
    }};
    li.appendChild(span);li.appendChild(btn);ul.appendChild(li);
  });
  const il=document.getElementById('imagelist');
  d.images.forEach(f=>{const li=document.createElement('li');li.textContent=f;il.appendChild(li);});
  if(d.wifiSSID) document.getElementById('wifiSSID').value=d.wifiSSID;
});
function saveSettings(){
  const s={wifiSSID:document.getElementById('wifiSSID').value,wifiPass:document.getElementById('wifiPass').value};
  fetch('/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(s)})
  .then(r=>r.json()).then(r=>{
    const el=document.getElementById('settingsStatus');el.style.display='block';
    if(r.ok){el.className='status success';el.textContent='Settings saved! Reconnect to new WiFi on next upload.';}
    else{el.className='status error';el.textContent=r.error||'Save failed';}
  });
}
function doSearch(){
  const q=document.getElementById('searchQ').value;
  if(!q)return;
  document.getElementById('searchResults').innerHTML='Searching...';
  fetch('/search?q='+encodeURIComponent(q)).then(r=>r.json()).then(d=>{
    const el=document.getElementById('searchResults');
    if(!d.results||d.results.length===0){el.innerHTML='No results found.';return;}
    let html='<p>'+d.results.length+' results in "'+d.book+'":</p><ul>';
    d.results.forEach(r=>{
      html+='<li>Ch '+(r.ch+1)+': ...'+r.ctx.replace(/</g,'&lt;')+'...</li>';
    });
    el.innerHTML=html+'</ul>';
  }).catch(e=>{document.getElementById('searchResults').innerHTML='Error: '+e;});
}
</script>
<div class="books">
  <h2>Search Book</h2>
  <p style="color:#666">Search the currently open book (open a book on the device first)</p>
  <input type="text" id="searchQ" placeholder="Enter search term..." style="width:70%">
  <button onclick="doSearch()">Search</button>
  <div id="searchResults" style="margin-top:10px"></div>
</div>
</body>
</html>
)rawliteral";

static bool isEpubFile(const String& filename) {
    String name = filename;
    name.toLowerCase();
    return name.endsWith(".epub");
}

static bool isSleepImageFile(const String& filename) {
    String name = filename;
    name.toLowerCase();
    return name.endsWith(".png") || name.endsWith(".jpg") || name.endsWith(".jpeg");
}

static void handleRoot() {
    _server.send(200, "text/html", UPLOAD_HTML);
}

static void handleUploadComplete() {
    if (_uploadFile) {
        _uploadFile.close();
    }
    _server.send(200, "text/html",
        "<html><body><h2>Upload complete!</h2>"
        "<p><a href='/'>Back to upload page</a></p></body></html>");
}

static void handleUploadData() {
    HTTPUpload& upload = _server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        String baseDir;
        if (isEpubFile(filename)) {
            baseDir = BOOKS_DIR;
        } else if (isSleepImageFile(filename)) {
            baseDir = SLEEP_IMAGES_DIR;
        } else {
            Serial.printf("Unsupported upload type: %s\n", filename.c_str());
            return;
        }
        String path = baseDir + filename;
        Serial.printf("Upload start: %s\n", path.c_str());
        _uploadFile = SD.open(path, FILE_WRITE);
        if (!_uploadFile) {
            Serial.println("Failed to open file for writing");
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (_uploadFile) {
            _uploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (_uploadFile) {
            Serial.printf("Upload complete: %u bytes\n", upload.totalSize);
            _uploadFile.close();
        }
    }
}

static void handleList() {
    String json = "{\"books\":[";
    File dir = SD.open(BOOKS_DIR);
    bool first = true;
    if (dir) {
        File entry;
        while ((entry = dir.openNextFile())) {
            if (!entry.isDirectory()) {
                String name = String(entry.name());
                if (isEpubFile(name)) {
                    if (!first) json += ",";
                    json += "\"" + String(entry.name()) + "\"";
                    first = false;
                }
            }
            entry.close();
        }
        dir.close();
    }
    json += "],\"images\":[";
    dir = SD.open(SLEEP_IMAGES_DIR);
    first = true;
    if (dir) {
        File entry;
        while ((entry = dir.openNextFile())) {
            if (!entry.isDirectory()) {
                String name = String(entry.name());
                if (isSleepImageFile(name)) {
                    if (!first) json += ",";
                    json += "\"" + String(entry.name()) + "\"";
                    first = false;
                }
            }
            entry.close();
        }
        dir.close();
    }
    json += "],\"wifiSSID\":\"";
    json += settings_get().wifiSSID;
    json += "\"}";
    _server.send(200, "application/json", json);
}

static void handleSettings() {
    String body = _server.arg("plain");
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        _server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    Settings& s = settings_get();
    if (doc.containsKey("wifiSSID")) s.wifiSSID = doc["wifiSSID"].as<String>();
    if (doc.containsKey("wifiPass")) s.wifiPass = doc["wifiPass"].as<String>();
    settings_save();
    Serial.printf("WiFi settings updated: SSID=%s\n", s.wifiSSID.c_str());
    _server.send(200, "application/json", "{\"ok\":true}");
}

static void handleDelete() {
    String body = _server.arg("plain");
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err || !doc.containsKey("filename")) {
        _server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid request\"}");
        return;
    }
    String filename = doc["filename"].as<String>();
    if (!filename.startsWith("/")) filename = "/" + filename;

    // Delete the book file
    String bookPath = String(BOOKS_DIR) + filename;
    if (!SD.exists(bookPath.c_str())) {
        _server.send(404, "application/json", "{\"ok\":false,\"error\":\"File not found\"}");
        return;
    }
    SD.remove(bookPath.c_str());
    Serial.printf("Deleted book: %s\n", bookPath.c_str());

    // Delete progress file if it exists
    String progressPath = String(PROGRESS_DIR) + filename + ".json";
    if (SD.exists(progressPath.c_str())) {
        SD.remove(progressPath.c_str());
        Serial.printf("Deleted progress: %s\n", progressPath.c_str());
    }

    // Delete library cache so it rebuilds on next scan
    const char* cachePath = "/books/.library_cache.json";
    if (SD.exists(cachePath)) {
        SD.remove(cachePath);
        Serial.println("Removed library cache");
    }

    _server.send(200, "application/json", "{\"ok\":true}");
}

static void handleSearch() {
    if (!_reader || _reader->getTitle().length() == 0) {
        _server.send(400, "application/json", "{\"ok\":false,\"error\":\"No book open\"}");
        return;
    }
    String query = _server.arg("q");
    if (query.length() == 0) {
        _server.send(400, "application/json", "{\"ok\":false,\"error\":\"Empty query\"}");
        return;
    }
    query.toLowerCase();

    String json = "{\"book\":\"" + _reader->getTitle() + "\",\"results\":[";
    int totalChapters = _reader->getTotalChapters();
    int found = 0;
    const int MAX_RESULTS = 20;

    for (int ch = 0; ch < totalChapters && found < MAX_RESULTS; ch++) {
        yield();
        String text = _reader->getParser().getChapterText(ch);
        String lower = text;
        lower.toLowerCase();
        int pos = 0;
        while (pos >= 0 && found < MAX_RESULTS) {
            pos = lower.indexOf(query, pos);
            if (pos < 0) break;
            // Extract context: 40 chars around match
            int ctxStart = max(0, pos - 20);
            int ctxEnd = min((int)text.length(), pos + (int)query.length() + 20);
            String context = text.substring(ctxStart, ctxEnd);
            context.replace("\"", "'");
            context.replace("\n", " ");
            if (found > 0) json += ",";
            json += "{\"ch\":" + String(ch) + ",\"pos\":" + String(pos) +
                    ",\"ctx\":\"" + context + "\"}";
            found++;
            pos += query.length();
        }
        text = String();  // free
    }
    json += "]}";
    _server.send(200, "application/json", json);
    Serial.printf("Search '%s': %d results\n", query.c_str(), found);
}

void wifi_upload_init() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void wifi_upload_start() {
    if (_running) return;

    const Settings& s = settings_get();
    Serial.printf("Connecting to WiFi: %s\n", s.wifiSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(s.wifiSSID.c_str(), s.wifiPass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi connection failed");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return;
    }

    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());

    _server.on("/", HTTP_GET, handleRoot);
    _server.on("/upload", HTTP_POST, handleUploadComplete, handleUploadData);
    _server.on("/list", HTTP_GET, handleList);
    _server.on("/settings", HTTP_POST, handleSettings);
    _server.on("/delete", HTTP_POST, handleDelete);
    _server.on("/search", HTTP_GET, handleSearch);
    _server.begin();
    _running = true;
}

void wifi_upload_stop() {
    if (!_running) return;
    _server.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    _running = false;
}

void wifi_upload_handle() {
    if (_running) {
        _server.handleClient();
    }
}

bool wifi_upload_running() {
    return _running;
}

String wifi_upload_ip() {
    if (_running && WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "Not connected";
}
