#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <iostream>

#ifndef NDEBUG
    #define DPRINTF(message, ...) fprintf(stderr, message, __VA_ARGS__);
#else
    #define DPRINTF(x) {};
#endif



using namespace std;
using namespace cv;

static void help()
{
    cout << "\nThis program demonstrates GrabCut segmentation -- select an object in a region\n"
         "and then grabcut will attempt to segment it out.\n"
         "Call:\n"
         "./grabcut <image_name>\n"
         "\nSelect a rectangular area around the object you want to segment\n" <<
         "\nHot keys: \n"
         "\tESC - quit the program\n"
         "\tr - restore the original image\n"
         "\tn - next iteration\n"
         "\n"
         "\tleft mouse button - set rectangle\n"
         "\n"
         "\tCTRL+left mouse button - set GC_BGD pixels\n"
         "\tSHIFT+left mouse button - set CG_FGD pixels\n"
         "\n"
         "\tCTRL+right mouse button - set GC_PR_BGD pixels\n"
         "\tSHIFT+right mouse button - set CG_PR_FGD pixels\n" << endl;
}

// color definitions
const Scalar RED       = Scalar(0, 0, 255);
const Scalar PINK      = Scalar(230, 130, 255);
const Scalar BLUE      = Scalar(255, 0, 0);
const Scalar LIGHTBLUE = Scalar(255, 255, 160);
const Scalar GREEN     = Scalar(0, 255, 0);

const int BGD_KEY = CV_EVENT_FLAG_CTRLKEY;
const int FGD_KEY = CV_EVENT_FLAG_SHIFTKEY;

const int MAX_TOLERANCE = 100;

static void getBinMask(const Mat& comMask, Mat& binMask)
{
    if (comMask.empty() || comMask.type() != CV_8UC1) {
        CV_Error(CV_StsBadArg, "comMask is empty or has incorrect type (not CV_8UC1)");
    }
    if (binMask.empty() || binMask.rows != comMask.rows || binMask.cols != comMask.cols) {
        binMask.create(comMask.size(), CV_8UC1);
    }
    binMask = comMask & 1;
}

class GCApplication
{
public:
    // state flags
    enum { NOT_SET = 0, IN_PROCESS = 1, SET = 2 };

    // parameters for brush stroke painting
    static const int radius = 2;
    static const int thickness = -1;

    GCApplication(double tolerance = MAX_TOLERANCE) : tolerance(tolerance) {};
    void reset();
    void setImageAndWinName(const Mat& _image, const string& _winName);
    void showImage() const;
    void mouseClick(int event, int x, int y, int flags);
    int nextIter();

    /**
     * Sets tolerance for initial GMM estimization. The app will fallback
     * in an uninitiazed state with, but the rectangular and brush strokes
     * are kept.
     */
    void setTolerance(double tolerance);

    inline int getIterCount() const { return iterCount; }

private:
    void setRectInMask();
    void setLabelsInMask(int flags, Point p, bool isPr);

    const string* winName;
    const Mat* image;
    Mat mask;

    // temporary arrays fir the back- and foreground model. They will be used internally by GrabCut
    // and should not be modified during the interations
    Mat backgroundModel, foregroundModel;

    // state flags
    uchar rectState, labelsState, probablyLabelsState;
    bool isInitialized;

    // region of interest containing a segmented object. defined by user
    Rect rect;
    // vectors of user-marked pixels for fore- and background
    vector<Point> foregroundPixels, backgroundPixels, probablyForegroundPixels, probablyBackgroundPixels;
    int iterCount;

    double tolerance;
};

void GCApplication::reset()
{
    // reset the mask to all background
    if (!mask.empty()) {
        mask.setTo(Scalar::all(GC_BGD));
    }
    backgroundPixels.clear();
    foregroundPixels.clear();
    probablyBackgroundPixels.clear();
    probablyForegroundPixels.clear();

    isInitialized = false;
    rectState = NOT_SET;
    labelsState = NOT_SET;
    probablyLabelsState = NOT_SET;
    iterCount = 0;
}

void GCApplication::setImageAndWinName(const Mat& _image, const string& _winName)
{
    if (_image.empty() || _winName.empty()) {
        return;
    }
    image = &_image;
    winName = &_winName;
    mask.create(image->size(), CV_8UC1);
    reset();
}

void GCApplication::showImage() const
{
    if (image->empty() || winName->empty()) {
        return;
    }

    Mat canvas;
    Mat binMask;

    if (!isInitialized) {
        image->copyTo(canvas);
    } else {
        // create initial binary mask
        getBinMask(mask, binMask);
        image->copyTo(canvas, binMask);
    }

    // draw each user-defined brush stroke
    for (int i = 0; i < backgroundPixels.size(); ++i) {
        circle(canvas, backgroundPixels[i], radius, BLUE, thickness);
    }
    for (int i = 0; i < foregroundPixels.size(); ++i) {
        circle(canvas, foregroundPixels[i], radius, RED, thickness);
    }
    for (int i = 0; i < probablyBackgroundPixels.size(); ++i) {
        circle(canvas, probablyBackgroundPixels[i], radius, LIGHTBLUE, thickness);
    }
    for (int i = 0; i < probablyForegroundPixels.size(); ++i) {
        circle(canvas, probablyForegroundPixels[i], radius, PINK, thickness);
    }


    if (rectState == IN_PROCESS || rectState == SET) {
        rectangle(canvas, Point(rect.x, rect.y), Point(rect.x + rect.width, rect.y + rect.height), GREEN, 2);
    }

    imshow(*winName, canvas);
}

void GCApplication::setRectInMask()
{
    assert(!mask.empty());
    mask.setTo(GC_BGD);
    rect.x = max(0, rect.x);
    rect.y = max(0, rect.y);
    rect.width = min(rect.width, image->cols - rect.x);
    rect.height = min(rect.height, image->rows - rect.y);
    (mask(rect)).setTo(Scalar(GC_PR_FGD));
}

void GCApplication::setLabelsInMask(int flags, Point p, bool isPr)
{
    vector<Point> *bpxls, *fpxls;
    uchar bvalue, fvalue;
    if (!isPr) {
        bpxls = &backgroundPixels;
        fpxls = &foregroundPixels;
        bvalue = GC_BGD;
        fvalue = GC_FGD;
    } else {
        bpxls = &probablyBackgroundPixels;
        fpxls = &probablyForegroundPixels;
        bvalue = GC_PR_BGD;
        fvalue = GC_PR_FGD;
    }
    if (flags & BGD_KEY) {
        bpxls->push_back(p);
        circle(mask, p, radius, bvalue, thickness);
    }
    if (flags & FGD_KEY) {
        fpxls->push_back(p);
        circle(mask, p, radius, fvalue, thickness);
    }
}

void GCApplication::mouseClick(int event, int x, int y, int flags)
{
    // TODO add bad args check
    switch (event) {
        case CV_EVENT_LBUTTONDOWN: { // set rect or GC_BGD(GC_FGD) labels
            bool isb = (flags & BGD_KEY) != 0,
                 isf = (flags & FGD_KEY) != 0;

            if (rectState == NOT_SET && !isb && !isf) {
                rectState = IN_PROCESS;
                rect = Rect(x, y, 1, 1);
            }
            if ((isb || isf) && rectState == SET) {
                labelsState = IN_PROCESS;
            }
        }
        break;
        case CV_EVENT_RBUTTONDOWN: { // set GC_PR_BGD(GC_PR_FGD) labels
            bool isb = (flags & BGD_KEY) != 0,
                 isf = (flags & FGD_KEY) != 0;
            if ((isb || isf) && rectState == SET) {
                probablyLabelsState = IN_PROCESS;
            }
        }
        break;
        case CV_EVENT_LBUTTONUP:
            if (rectState == IN_PROCESS) {
                rect = Rect(Point(rect.x, rect.y), Point(x, y));
                rectState = SET;
                setRectInMask();
                assert(backgroundPixels.empty() && foregroundPixels.empty() && probablyBackgroundPixels.empty() && probablyForegroundPixels.empty());
                showImage();
            }
            if (labelsState == IN_PROCESS) {
                setLabelsInMask(flags, Point(x, y), false);
                labelsState = SET;
                showImage();
            }
            break;
        case CV_EVENT_RBUTTONUP:
            if (probablyLabelsState == IN_PROCESS) {
                setLabelsInMask(flags, Point(x, y), true);
                probablyLabelsState = SET;
                showImage();
            }
            break;
        case CV_EVENT_MOUSEMOVE:
            if (rectState == IN_PROCESS) {
                rect = Rect(Point(rect.x, rect.y), Point(x, y));
                assert(backgroundPixels.empty() && foregroundPixels.empty() && probablyBackgroundPixels.empty() && probablyForegroundPixels.empty());
                showImage();
            } else if (labelsState == IN_PROCESS) {
                setLabelsInMask(flags, Point(x, y), false);
                showImage();
            } else if (probablyLabelsState == IN_PROCESS) {
                setLabelsInMask(flags, Point(x, y), true);
                showImage();
            }
            break;
        default:
            break;
    }
}

int GCApplication::nextIter()
{
    cerr << "tolerance: " << tolerance << endl;

    if (isInitialized) {
        grabCut(*image, mask, rect, backgroundModel, foregroundModel, 1);
    } else {
        // if the application not initialized and the rectangular is not set up be the user
        // we do nothing
        if (rectState != SET) {
            return iterCount;
        }

        // do the initial iteration
        //
        // if the user provides brush strokes, use them as mask for the initial iteration
        if (labelsState == SET || probablyLabelsState == SET) {
            grabCut(*image, mask, rect, backgroundModel, foregroundModel, 1, GC_INIT_WITH_MASK);
        } else {
            grabCut(*image, mask, rect, backgroundModel, foregroundModel, 1, GC_INIT_WITH_RECT);
        }
        // after the initial iteration, the application is initialized
        isInitialized = true;
    }
    iterCount++;

    // delete all brush strokes
    backgroundPixels.clear();
    foregroundPixels.clear();
    probablyBackgroundPixels.clear();
    probablyForegroundPixels.clear();

    return iterCount;
}


void GCApplication::setTolerance(double _tolerance)
{
    tolerance = _tolerance;
    isInitialized = false;
    iterCount = 0;

    this->showImage();
}

static void on_mouse(int event, int x, int y, int flags, void* gcapp)
{
    ((GCApplication*) gcapp)->mouseClick(event, x, y, flags);
}

static void on_trackbar(int value, void* gcapp)
{
    DPRINTF("on_trackbar: %d\n", value);
    ((GCApplication*) gcapp)->setTolerance((double) value / (double) MAX_TOLERANCE);
}

int main(int argc, char** argv)
{
    GCApplication gcapp;

    if (argc != 2) {
        help();
        return 1;
    }
    string filename = argv[1];
    if (filename.empty()) {
        cout << "\nDurn, couldn't read in " << argv[1] << endl;
        return 1;
    }
    Mat image = imread(filename, 1);
    if (image.empty()) {
        cout << "\n Durn, couldn't read image filename " << filename << endl;
        return 1;
    }

    help();

    int toleranceSlider = 50;
    const string winName = "image";
    namedWindow(winName, WINDOW_AUTOSIZE);
    setMouseCallback(winName, on_mouse, &gcapp);
    createTrackbar("tolerance", winName, &toleranceSlider, MAX_TOLERANCE, on_trackbar, &gcapp);

    gcapp.setImageAndWinName(image, winName);
    gcapp.showImage();

    while (true) {
        int c = waitKey(0);
        switch ((char) c) {
            case '\x1b':
                cout << "Exiting ..." << endl;
                goto exit_main;
            case 'r':
                cout << endl;
                gcapp.reset();
                gcapp.showImage();
                break;
            case 'n':
                int iterCount = gcapp.getIterCount();
                cout << "<" << iterCount << "... ";
                int newIterCount = gcapp.nextIter();
                if (newIterCount > iterCount) {
                    gcapp.showImage();
                    cout << iterCount << ">" << endl;
                } else {
                    cout << "rect must be determined>" << endl;
                }
                break;
        }
    }

exit_main:
    destroyWindow(winName);
    return 0;
}
