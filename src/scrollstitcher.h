#pragma once

#include <QImage>
#include <QList>

#include <vector>

// Builds one tall image from successive captures of a region whose content scrolls down.
// Each frame is matched against the previous one: first by exact per-row hashes, and when no
// offset fits exactly (browsers scroll by fractions of a pixel, so anti-aliased text never
// repeats exactly) by per-row brightness profiles with sub-pixel interpolation, ignoring the
// worst-fitting rows (hover highlights, small animations). Rows that stay put at the top and
// bottom (sticky headers/footers) are excluded from matching and kept only once.
class ScrollStitcher
{
public:
    enum class Status {
        Started,   // first frame taken as is
        Appended,  // new rows added
        Unchanged, // nothing scrolled
        Lost,      // no overlap with the previous frame (scrolled too fast, or upwards)
        Full,      // maximum height reached; further frames are ignored
    };

    explicit ScrollStitcher(int maxHeight = 30000);

    Status add(const QImage &frame);

    int height() const { return m_height; }
    QImage result() const;
    QImage tail(int rows) const; // the last rows of the result, for previews

private:
    struct Row
    {
        quint64 hash;
        bool uniform;     // a single color; matches anywhere, so it carries no evidence
        bool informative; // enough contrast to count in the tolerant search
    };
    // Per-row brightness profiles: coarse ones for a quick search, fine ones to decide.
    struct Profiles
    {
        std::vector<float> coarse;
        std::vector<float> fine;
    };

    QList<Row> rowsOf(const QImage &frame) const;
    Profiles profilesOf(const QImage &frame) const;
    int findOffset(const QList<Row> &rows, int bandStart, int bandEnd) const;
    // The tolerant search: an offset, 0 when the content did not move, -1 when nothing fits.
    int findOffsetApprox(const QList<Row> &rows, const Profiles &profiles, int bandStart, int bandEnd) const;
    void appendRows(const QImage &frame, int from, int count);

    int m_maxHeight;
    QImage m_previous;
    QList<Row> m_previousRows;
    Profiles m_previousProfiles;
    QImage m_canvas; // capacity grows geometrically; only the first m_height rows are used
    int m_height = 0;
};

// Long screenshot in whichever direction the content scrolls: down, or right (wide tables,
// timelines). Frames go to a vertical stitcher and, transposed, to a second one; the first to
// append decides the direction and the other is dropped.
class TwoWayStitcher
{
public:
    enum class Direction { Unknown, Down, Right };

    // Longest result: maxHeight rows when scrolling down, maxWidth columns when scrolling right.
    TwoWayStitcher(int maxHeight, int maxWidth);

    ScrollStitcher::Status add(const QImage &frame);
    Direction direction() const { return m_direction; }
    int length() const; // pixels stitched along the scrolling direction
    QImage result() const;
    // The newest part (rows at the bottom, or columns at the right), at most `pixels` long.
    QImage tail(int pixels) const;

private:
    ScrollStitcher m_down;
    ScrollStitcher m_right; // sees transposed frames: new columns on the right arrive at the bottom
    Direction m_direction = Direction::Unknown;
};
