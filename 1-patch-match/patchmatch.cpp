#include <opencv2/opencv.hpp>
#include <cstdlib>     // rand
#include <iostream>
#include <numeric> // numeric_limits
#include "patchmatch.hpp"

using namespace std;
using namespace cv;

float ssd(const Mat& image1, const Point2i& center1, const Mat& image2, const Point2i& center2, const int radius, const float halt)
{
    float sum = 0;

    for (int row = -radius; row <= radius; ++row) {
        for (int col = -radius; col <= radius; ++col) {
            // cout << (row + center1.y) << "," << (col + center1.x) << endl;
            // cout << (row + center2.y) << "," << (col + center2.x) << endl;
    
            const uchar gray1 = image1.at<uchar>(row + center1.y, col + center1.x);
            const uchar gray2 = image2.at<uchar>(row + center2.y, col + center2.x);

            const float diff  = gray1 - gray2;

            sum += diff * diff;

            // early termination
            if (sum > halt) {
                return sum;
            }

        }
    }

//    cout << sum << endl;

    return sum;
}


void flow2rgb(const Mat& flow, Mat& rgb)
{
    // extract x and y channels
    Mat xy[2]; // x,y

    split(flow, xy);

    // calculate angle and magnitude for the HSV color wheel
    Mat magnitude;
    Mat angle;

    cartToPolar(xy[0], xy[1], magnitude, angle, true);

    // translate magnitude to range [0,1]
    double mag_max;
    minMaxLoc(magnitude, 0, &mag_max);

    magnitude.convertTo(
        magnitude,    // output matrix
        -1,           // type of the ouput matrix, if negative same type as input matrix
        1.0 / mag_max // scaling factor
    );

    // build HSV image (hue-saturation-value)
    Mat _hsv[3]; // array of three matrices - one for each channel
    Mat hsv;

    // create separate channels
    _hsv[0] = angle;                           // H (hue)              [0,360]
    _hsv[1] = magnitude;                       // S (saturation)       [0,1]
    _hsv[2] = Mat::ones(angle.size(), CV_32F); // V (brigthness value) [0,1]

    // merge the three components to a three channel HSV image
    merge(_hsv, 3, hsv);

    // convert to BGR
    cvtColor(hsv, rgb, cv::COLOR_HSV2BGR);
}



PatchMatch::PatchMatch(int maxoffset, int match_radius, int iterations, float search_ratio, int search_radius) :
    // Parameters
    iterations(iterations),
    maxoffset(maxoffset),
    match_radius(match_radius),
    search_ratio(search_ratio),
    border(match_radius),
    max_search_radius(search_radius == -1)
{
    this->search_radius = search_radius;
}

void PatchMatch::match(const Mat& image1, const Mat& image2, Mat& dest)
{
    nrows = image1.rows;
    ncols = image1.cols;

    if (max_search_radius == true) {
        search_radius = min(nrows, ncols);
    }

    cout << "search_radius: " << search_radius << endl;

    // create an empty matrix with the same x-y dimensions like the first
    // image but with two channels. Each channel stands for an x/y offset
    // of a pixel at this position.
    costs = Mat::zeros(nrows, ncols, CV_32FC1);
    flow  = Mat::zeros(nrows, ncols, CV_32FC2); // 2-channel 32-bit floating point

    initialize(image1, image2);

    // for (niterations = 0; niterations < iterations; ++niterations) {

    //     cout << "iteration " << niterations << endl;

    //     for (int row = match_radius; row < nrows - match_radius ; ++row) {

    //         // cerr << row << endl;

    //         for (int col = match_radius; col < ncols - match_radius; ++col) {

    //             // cout << "    " << col << endl;

    //             propagate(image1, image2, row, col);
    //             // random_search(image1, image2, row, col);
    //         }
    //     }
    // }

    flow.copyTo(dest);
}

void PatchMatch::initialize(const Mat& image1, const Mat& image2)
{
    Point2i offset;
    Point2i index;
    Point2i center;

    for (int row = border; row < nrows - border ; ++row) {
        for (int col = border; col < ncols - border ; ++col) {
            index.x = col;
            index.y = row;

            // cerr << "index " << index << endl;

            while (true) {
                offset.x = rand() % (2 * maxoffset) - maxoffset;
                offset.y = rand() % (2 * maxoffset) - maxoffset;

                // cerr << "offset " << offset << endl;

                center = index + offset;

                // check if the center is inside the other image
                if (border <= center.x && center.x < ncols - border &&
                    border <= center.y && center.y < nrows - border
                ) {
                    // cerr << "break" << endl;
                    break;
                } else {
                    // cerr << index << " + " << offset << " = " << center << endl;

                    // cerr << border << " <= " << center.x << " && " << center.x << " < " << ncols << " - " << border << endl;
                    // cerr << border << " <= " << center.y << " && " << center.y << " < " << nrows << " - " << border << endl;
                }
            }

            // cerr << "offset " << offset << endl;

            flow.at<Point2f>(row, col) = offset;
            costs.at<float>(row, col) = ssd(image1, index, image2, center, match_radius);
        }
    }
}

void PatchMatch::propagate(const cv::Mat &image1, const cv::Mat &image2, const int row, const int col)
{
    // switch between top and left neighbor in even iterations and
    // right bottom neighbor in odd iterations
    int direction = (niterations % 2 == 0) ? 1 : -1;

    // top or bottom
    if (costs.at<float>(row + direction, col) < costs.at<float>(row, col)) {
        flow.at<Point2f>(row, col) = flow.at<Point2f>(row + direction, col);
    }
    // left or right
    if (costs.at<float>(row, col + direction) < costs.at<float>(row, col)) {
        flow.at<Point2f>(row, col) = flow.at<Point2f>(row, col + direction);
    }
}

static Point2f SEARCH_FIELD[8] = {
    Point2f(-1, -1), Point2f(-1, 0), Point2f(-1, 1),
    Point2f( 0, -1),                 Point2f( 0, 1),
    Point2f( 1, -1), Point2f( 1, 0), Point2f( 1, 1)
};

void PatchMatch::random_search(const cv::Mat &image1, const cv::Mat &image2, const int row, const int col)
{
    int i = 0;

    while (true) {
        const float distance = search_radius * pow(search_ratio, i++);

        // halt condition. search radius must not be smaller
        // than one pixel
        if (distance < 1) {
            break;
        }

        // cout << col << " " <<  distance << endl;

        const Point2f direction = SEARCH_FIELD[rand() % 8];
        const Point2f offset(direction.x * distance,
                             direction.y * distance);

        const Point2i center(row + (int) offset.x,
                             col + (int) offset.y);

        // check if we are inside image range
        if (match_radius < center.x && center.x < nrows - match_radius &&
            match_radius < center.y && center.y < ncols - match_radius) {

             float q = ssd(image1, Point2i(row, col), image2, center, search_radius, costs.at<float>(row, col));

//            float q = 0;

            // only update if the current match is better
//            if (q < quality.at<float>(row, col)) {
//                quality.at<float>(row, col) = q;
//                flow.at<Point2f>(row, col) = offset;
//            }
        }
    }
}
