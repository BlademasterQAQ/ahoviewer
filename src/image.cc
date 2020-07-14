#include <cctype>
#include <glib.h>
#include <gtkmm.h>
#include <giomm.h>
#include <iostream>

#include "image.h"
using namespace AhoViewer;

#include "settings.h"

std::string Image::ThumbnailDir = Glib::build_filename(Glib::get_user_cache_dir(), "thumbnails", "normal");

bool Image::is_valid(const std::string &path)
{
    return gdk_pixbuf_get_file_info(path.c_str(), 0, 0) != NULL || is_webm(path);
}

bool Image::is_valid_extension(const std::string &path)
{
    std::string ext = path.substr(path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

#ifdef HAVE_GSTREAMER
    if (ext == "webm")
        return true;
#endif // HAVE_GSTREAMER

    bool r = false;

    for (Gdk::PixbufFormat i : Gdk::Pixbuf::get_formats())
    {
        gchar **extensions = gdk_pixbuf_format_get_extensions(i.gobj());
        for (int j = 0; extensions[j] != NULL; ++j)
        {
            if (strcmp(ext.c_str(), extensions[j]) == 0)
                r = true;
        }

        g_strfreev(extensions);

        if (r)
            break;
    }

    return r;
}

bool Image::is_webm(const std::string &path)
{
#ifdef HAVE_GSTREAMER
    bool uncertain;
    std::string ct       = Gio::content_type_guess(path, NULL, 0, uncertain);
    std::string mimeType = Gio::content_type_get_mime_type(ct);

    return mimeType == "video/webm";
#else
    (void)path;
    return false;
#endif // HAVE_GSTREAMER
}

const Glib::RefPtr<Gdk::Pixbuf>& Image::get_missing_pixbuf()
{
    static const Glib::RefPtr<Gdk::Pixbuf> pixbuf =
        Gtk::IconTheme::get_default()->load_icon("image-missing", 48,
            Gtk::ICON_LOOKUP_USE_BUILTIN | Gtk::ICON_LOOKUP_GENERIC_FALLBACK);

    return pixbuf;
}

static void* _def_bitmap_create(int width, int height)
{
    return new unsigned char[width * height * 4];
}

static void _def_bitmap_destroy(void *bitmap)
{
    if (bitmap)
    {
        delete[] static_cast<unsigned char*>(bitmap);
        bitmap = nullptr;
    }
}

static unsigned char* _def_bitmap_get_buffer(void *bitmap)
{
    return static_cast<unsigned char*>(bitmap);
}

Image::Image(const std::string &path)
  : m_isWebM(Image::is_webm(path)),
    m_Loading(true),
    m_Path(path),
    m_GIFanim(nullptr),
    m_GIFcurFrame(0),
    m_GIFcurLoop(1)
{
    m_BitmapCallbacks.bitmap_create      = _def_bitmap_create;
    m_BitmapCallbacks.bitmap_destroy     = _def_bitmap_destroy;
    m_BitmapCallbacks.bitmap_get_buffer  = _def_bitmap_get_buffer;
    m_BitmapCallbacks.bitmap_test_opaque = nullptr;
    m_BitmapCallbacks.bitmap_set_opaque  = nullptr;
    m_BitmapCallbacks.bitmap_modified    = nullptr;
}

Image::~Image()
{
    if (m_GIFanim)
    {
        gif_finalise(m_GIFanim);
        delete m_GIFanim;
        m_GIFanim = nullptr;
    }
}

std::string Image::get_filename() const
{
    return Glib::build_filename(
            Glib::path_get_basename(Glib::path_get_dirname(m_Path)),
            Glib::path_get_basename(m_Path));
}

const Glib::RefPtr<Gdk::Pixbuf>& Image::get_pixbuf()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    // Get the current frame without advancing
    if (m_GIFanim && m_GIFanim->frame_count > 0)
        m_Pixbuf = get_gif_frame_pixbuf(false);

    return m_Pixbuf;
}

const Glib::RefPtr<Gdk::Pixbuf>& Image::get_gif_frame_pixbuf(const bool advance)
{
    gif_result result = gif_decode_frame(m_GIFanim, m_GIFcurFrame);

    if (result == GIF_OK)
    {
        m_Pixbuf = Gdk::Pixbuf::create_from_data(
            static_cast<unsigned char*>(m_GIFanim->frame_image), Gdk::COLORSPACE_RGB, true, 8,
            m_GIFanim->width, m_GIFanim->height, (m_GIFanim->width * 4 + 3) & ~3);

        // Handle frame advacing and looping.
        if (m_GIFanim->frame_count > 1 && advance)
        {
            // Currently on the last frame, reset to first frame unless we finished
            // the final loop
            if (m_GIFcurFrame == static_cast<int>(m_GIFanim->frame_count) - 1 && m_GIFcurLoop != m_GIFanim->loop_count)
            {
                // Only increment the loop counter if it's not looping forever.
                if (m_GIFanim->loop_count > 0)
                    ++m_GIFcurLoop;
                m_GIFcurFrame = 0;
            }
            else if (m_GIFcurFrame < static_cast<int>(m_GIFanim->frame_count) - 1)
            {
                ++m_GIFcurFrame;
            }
        }
    }
    else
    {
        std::cerr << "Error while decoding GIF frame " << m_GIFcurFrame << " of " << m_Path << std::endl
                  << "gif_result: " << result << std::endl;
    }

    return m_Pixbuf;
}

const Glib::RefPtr<Gdk::Pixbuf>& Image::get_thumbnail(Glib::RefPtr<Gio::Cancellable> c)
{
    if (m_ThumbnailPixbuf)
        return m_ThumbnailPixbuf;

#ifdef __linux__
    std::string thumbFilename = Glib::Checksum::compute_checksum(
            Glib::Checksum::CHECKSUM_MD5, Glib::filename_to_uri(m_Path)) + ".png";
    m_ThumbnailPath = Glib::build_filename(ThumbnailDir, thumbFilename);

    if (is_valid(m_ThumbnailPath))
    {
        Glib::RefPtr<Gdk::Pixbuf> pixbuf = create_pixbuf_at_size(m_ThumbnailPath,
                                                    ThumbnailSize, ThumbnailSize, c);

        if (pixbuf)
        {
            struct stat fileInfo;
            std::string s = pixbuf->get_option("tEXt::Thumb::MTime");

            // Make sure the file hasn't been modified since this thumbnail was created
            if (!s.empty())
            {
                time_t mtime = strtol(s.c_str(), nullptr, 10);

                if ((stat(m_Path.c_str(), &fileInfo) == 0) && fileInfo.st_mtime == mtime)
                    m_ThumbnailPixbuf = pixbuf;
            }
        }
    }
#endif // __linux__

    if (!m_ThumbnailPixbuf)
        create_thumbnail(c);

    return m_ThumbnailPixbuf;
}

void Image::load_pixbuf(Glib::RefPtr<Gio::Cancellable> c)
{
    if (!m_Pixbuf && !m_isWebM)
    {
        Glib::RefPtr<Gio::File> file = Gio::File::create_for_path(m_Path);

        std::array<unsigned char, 4> data;
        file->read()->read(&data, 4);

        if (is_gif(data.data()))
        {
            gif_result result;
            char *gif_data;
            gsize bufSize;
            m_GIFanim = new gif_animation;

            gif_create(m_GIFanim, &m_BitmapCallbacks);

            // returns false if c was cancelled
            if (file->load_contents(c, gif_data, bufSize))
            {
                do {
                    result = gif_initialise(m_GIFanim, bufSize, reinterpret_cast<unsigned char*>(gif_data));
                    if (result != GIF_OK && result != GIF_WORKING)
                    {
                        std::cerr << "Error while loading GIF " << m_Path << std::endl
                                  << "gif_result: " << result << std::endl;
                        break;
                    }
                    else if (result == GIF_OK)
                    {
                        // Load the first frame
                        m_Pixbuf = get_gif_frame_pixbuf(false);
                    }
                } while (result != GIF_OK);
            }
        }
        else
        {
            Glib::RefPtr<Gdk::Pixbuf> p = Gdk::Pixbuf::create_from_stream(file->read(), c);

            if (c->is_cancelled())
                return;

            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_Pixbuf = p;
            }
        }

        m_Loading = false;
        m_SignalPixbufChanged();
    }
}

void Image::reset_pixbuf()
{
    m_Loading = true;
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Pixbuf.reset();

    if (m_GIFanim)
    {
        gif_finalise(m_GIFanim);
        delete m_GIFanim;

        m_GIFanim = nullptr;
        reset_gif_animation();
    }
}

void Image::reset_gif_animation()
{
    m_GIFcurFrame = 0;
    m_GIFcurLoop  = 1;
}

bool Image::get_gif_finished_looping() const
{
    return m_GIFanim &&
           m_GIFcurLoop == m_GIFanim->loop_count &&
           m_GIFcurFrame == static_cast<int>(m_GIFanim->frame_count) - 1;
}

unsigned int Image::get_gif_frame_delay() const
{
    if (!m_GIFanim)
        return 0;
    int delay = m_GIFanim->frames[m_GIFcurFrame].frame_delay;
    // libnsgif stores delay in centiseconds, convert it to milliseconds.
    // if delay is 0, use a 100ms delay by default
    return delay ? delay * 10 : 100;
}

// This assumes data's length is at least 3
bool Image::is_gif(const unsigned char *data)
{
    return data[0] == 'G' &&
           data[1] == 'I' &&
           data[2] == 'F';
}

void Image::create_thumbnail(Glib::RefPtr<Gio::Cancellable> c, bool save)
{
    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    save = save && Settings.get_bool("SaveThumbnails");

    if (m_isWebM)
    {
        pixbuf = create_webm_thumbnail(128, 128);

#ifdef __linux__
        if (pixbuf && save)
            save_thumbnail(pixbuf, "video/webm");
#endif // __linux__
    }
    else
    {
        pixbuf = create_pixbuf_at_size(m_Path, 128, 128, c);

        if (c->is_cancelled() || !pixbuf)
            return;

#ifdef __linux__
        int w, h;
        GdkPixbufFormat *format = gdk_pixbuf_get_file_info(m_Path.c_str(), &w, &h);

        if (save && format && (w > 128 || h > 128))
        {
            gchar **mimeTypes = gdk_pixbuf_format_get_mime_types(format);
            save_thumbnail(pixbuf, mimeTypes[0]);
            g_strfreev(mimeTypes);
        }
#endif // __linux__
    }

    if (pixbuf && !c->is_cancelled())
        m_ThumbnailPixbuf = scale_pixbuf(pixbuf, ThumbnailSize, ThumbnailSize);
}

Glib::RefPtr<Gdk::Pixbuf> Image::create_pixbuf_at_size(const std::string &path,
                                                       const int w, const int h,
                                                       Glib::RefPtr<Gio::Cancellable> c) const
{
    Glib::RefPtr<Gio::File> file = Gio::File::create_for_path(path);
    Glib::RefPtr<Gdk::Pixbuf> pixbuf;

    try
    {
        pixbuf = Gdk::Pixbuf::create_from_stream_at_scale(file->read(), w, h, true, c);
    }
    catch (...)
    {
        if (!c->is_cancelled())
            std::cerr << "Error while loading thumbnail " << path
                      << " for " << get_filename() << std::endl;
    }

    return pixbuf;
}

Glib::RefPtr<Gdk::Pixbuf> Image::scale_pixbuf(Glib::RefPtr<Gdk::Pixbuf> &pixbuf,
                                              const int w, const int h) const
{
    double r = std::min(static_cast<double>(w) / pixbuf->get_width(),
                        static_cast<double>(h) / pixbuf->get_height());

    return pixbuf->scale_simple(std::max(pixbuf->get_width() * r, 20.0),
                                std::max(pixbuf->get_height() * r, 20.0),
                                Gdk::INTERP_BILINEAR);
}

Glib::RefPtr<Gdk::Pixbuf> Image::create_webm_thumbnail(int w, int h) const
{
    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
#ifdef HAVE_GSTREAMER
    gint64 dur, pos;
    GstSample *sample;
    GstMapInfo map;

    std::string des = Glib::ustring::compose("uridecodebin uri=%1 ! videoconvert ! videoscale ! "
           "appsink name=sink caps=\"video/x-raw,format=RGB,width=%2,pixel-aspect-ratio=1/1\"",
           Glib::filename_to_uri(m_Path).c_str(), w);
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(des.c_str(), &error);

    if (error != nullptr)
    {
        std::cerr << "create_webm_thumbnail: could not construct pipeline: " << error->message << std::endl;
        g_error_free(error);

        if (pipeline)
            gst_object_unref(pipeline);

        return pixbuf;
    }

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");

    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    gst_element_get_state(pipeline, NULL, NULL, 5 * GST_SECOND);
    gst_element_query_duration(pipeline, GST_FORMAT_TIME, &dur);

    static auto is_pixbuf_interesting = [](Glib::RefPtr<Gdk::Pixbuf> &p)
    {
        size_t len      = p->get_rowstride() * p->get_height();
        guint8 *buf     = p->get_pixels();
        double xbar     = 0.0,
               variance = 0.0;

        for (size_t i = 0; i < len; ++i)
            xbar += static_cast<double>(buf[i]);
        xbar /= static_cast<double>(len);

        for (size_t i = 0; i < len; ++i)
            variance += std::pow(static_cast<double>(buf[i]) - xbar, 2);

        return variance > 256.0;
    };

    // Looks at up to 6 different frames (unless duration is -1)
    // for a pixbuf that is "interesting"
    for (auto offset : { 1.0 / 3.0, 2.0 / 3.0, 0.1, 0.5, 0.9 })
    {
        pos = dur == -1 ? 1 * GST_SECOND : dur / GST_MSECOND * offset * GST_MSECOND;

        if (!gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_FLUSH), pos))
            break;

        g_signal_emit_by_name(sink, "pull-preroll", &sample, NULL);

        if (sample)
        {
            GstBuffer *buffer;
            GstCaps *caps;
            GstStructure *s;

            caps = gst_sample_get_caps(sample);
            if (!caps)
                break;

            s = gst_caps_get_structure(caps, 0);

            gst_structure_get_int(s, "width", &w);
            gst_structure_get_int(s, "height", &h);

            buffer = gst_sample_get_buffer(sample);
            gst_buffer_map(buffer, &map, GST_MAP_READ);
            pixbuf = Gdk::Pixbuf::create_from_data(map.data,
                                                    Gdk::COLORSPACE_RGB, false, 8, w, h,
                                                    GST_ROUND_UP_4(w * 3));

            /* save the pixbuf */
            gst_buffer_unmap (buffer, &map);

            if (dur == -1 || is_pixbuf_interesting(pixbuf))
                break;
        }
    }

    gst_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
#else
    (void)w;
    (void)h;
#endif // HAVE_GSTREAMER
    return pixbuf;
}

void Image::save_thumbnail(Glib::RefPtr<Gdk::Pixbuf> &pixbuf, const gchar *mimeType) const
{
    struct stat fileInfo;
    if (stat(m_Path.c_str(), &fileInfo) == 0)
    {
        const std::vector<Glib::ustring> opts =
        {
            "tEXt::Thumb::URI",
            "tEXt::Thumb::MTime",
            "tEXt::Thumb::Size",
            "tEXt::Thumb::Image::Mimetype",
            "tEXt::Software"
        },
        vals =
        {
            Glib::filename_to_uri(m_Path),     // URI
            std::to_string(fileInfo.st_mtime), // MTime
            std::to_string(fileInfo.st_size),  // Size
            mimeType,                          // Mimetype
            PACKAGE                            // Software
        };

        if (!Glib::file_test(ThumbnailDir, Glib::FILE_TEST_EXISTS))
            g_mkdir_with_parents(ThumbnailDir.c_str(), 0700);

        gchar *buf;
        gsize bufSize;
        pixbuf->save_to_buffer(buf, bufSize, "png", opts, vals);

        if (bufSize > 0)
        {
            try
            {
                Glib::file_set_contents(m_ThumbnailPath, buf, bufSize);
                chmod(m_ThumbnailPath.c_str(), S_IRUSR | S_IWUSR);
            }
            catch (const Glib::FileError &ex)
            {
                std::cerr << "Glib::file_set_contents: " << ex.what() << std::endl;
            }
        }

        g_free(buf);
    }
}
