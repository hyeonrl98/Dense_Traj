#include "DenseTrack.h"
#include "Initialize.h"
#include "Descriptors.h"
#include "OpticalFlow.h"

#include <windows.h>
#include <time.h>

using namespace cv;

int show_track = 0; // set show_track = 1, if you want to visualize the trajectories

void main() {

    TrackInfo trackInfo;
	DescInfo hogInfo, hofInfo, mbhInfo;

	InitTrackInfo(&trackInfo, track_length, init_gap);
	InitDescInfo(&hogInfo, 8, false, patch_size, nxy_cell, nt_cell);
	InitDescInfo(&hofInfo, 9, true, patch_size, nxy_cell, nt_cell);
	InitDescInfo(&mbhInfo, 8, false, patch_size, nxy_cell, nt_cell);

    if(show_track == 1)
		namedWindow("DenseTrack", 0);

    
	Mat image, prev_grey, grey;

	std::vector<float> fscales(0);
	std::vector<Size> sizes(0);

	std::vector<Mat> prev_grey_pyr(0), grey_pyr(0), flow_pyr(0);
	std::vector<Mat> prev_poly_pyr(0), poly_pyr(0); // for optical flow

	std::vector<std::list<Track> > xyScaleTracks;
	int init_counter = 0; // indicate when to detect new feature points

    int i, j, c;

    Mat image1;
    Mat image2;

    image1 = imread("image1.jpg");
    if (image1.empty()) {
        fprintf(stderr, "Could not read image file image1.jpg");
        return ;
    }

    // build pyramid for image 2
    image2 = imread("image2.jpg");
    if (image2.empty()) {
        fprintf(stderr, "Could not read image file image2.jpg");
        return ;
    }

    image.create(image1.size(), CV_8UC3);
    grey.create(image1.size(), CV_8UC1);
    prev_grey.create(image1.size(), CV_8UC1);

    InitPry(image1, fscales, sizes);

    BuildPry(sizes, CV_8UC1, prev_grey_pyr);
    BuildPry(sizes, CV_8UC1, grey_pyr);

    BuildPry(sizes, CV_32FC2, flow_pyr);
    BuildPry(sizes, CV_32FC(5), prev_poly_pyr);
    BuildPry(sizes, CV_32FC(5), poly_pyr);

    xyScaleTracks.resize(scale_num);

    image1.copyTo(image);
	cv::cvtColor(image, prev_grey, cv::COLOR_BGR2GRAY);

    for(int iScale = 0; iScale < scale_num; iScale++) {
        if(iScale == 0)
            prev_grey.copyTo(prev_grey_pyr[0]);
        else
            resize(prev_grey_pyr[iScale-1], prev_grey_pyr[iScale], prev_grey_pyr[iScale].size(), 0, 0, INTER_LINEAR);

        // dense sampling feature points
        std::vector<Point2f> points(0);
        DenseSample(prev_grey_pyr[iScale], points, quality, min_distance);

        // save the feature points
        std::list<Track>& tracks = xyScaleTracks[iScale];
        for(i = 0; i < points.size(); i++)
            tracks.push_back(Track(points[i], trackInfo, hogInfo, hofInfo, mbhInfo));
    }

    // compute polynomial expansion
    my::FarnebackPolyExpPyr(prev_grey, prev_poly_pyr, fscales, 7, 1.5);

    init_counter++;
    image2.copyTo(image);
    cv::cvtColor(image, grey, cv::COLOR_BGR2GRAY);

    // compute optical flow for all scales once
    my::FarnebackPolyExpPyr(grey, poly_pyr, fscales, 7, 1.5);
    my::calcOpticalFlowFarneback(prev_poly_pyr, poly_pyr, flow_pyr, 10, 2);    

    for(int iScale = 0; iScale < scale_num; iScale++) {
        if(iScale == 0)
            grey.copyTo(grey_pyr[0]);
        else
            resize(grey_pyr[iScale-1], grey_pyr[iScale], grey_pyr[iScale].size(), 0, 0, INTER_LINEAR);

        int width = grey_pyr[iScale].cols;
        int height = grey_pyr[iScale].rows;

        // compute the integral histograms
        DescMat* hogMat = InitDescMat(height+1, width+1, hogInfo.nBins);
        HogComp(prev_grey_pyr[iScale], hogMat->desc, hogInfo);

        DescMat* hofMat = InitDescMat(height+1, width+1, hofInfo.nBins);
        HofComp(flow_pyr[iScale], hofMat->desc, hofInfo);

        DescMat* mbhMatX = InitDescMat(height+1, width+1, mbhInfo.nBins);
        DescMat* mbhMatY = InitDescMat(height+1, width+1, mbhInfo.nBins);
        MbhComp(flow_pyr[iScale], mbhMatX->desc, mbhMatY->desc, mbhInfo);

        // track feature points in each scale separately
        std::list<Track>& tracks = xyScaleTracks[iScale];
        for (std::list<Track>::iterator iTrack = tracks.begin(); iTrack != tracks.end();) {
            int index = iTrack->index;
            Point2f prev_point = iTrack->point[index];
            int x = std::min<int>(std::max<int>(cvRound(prev_point.x), 0), width-1);
            int y = std::min<int>(std::max<int>(cvRound(prev_point.y), 0), height-1);

            Point2f point;
            point.x = prev_point.x + flow_pyr[iScale].ptr<float>(y)[2*x];
            point.y = prev_point.y + flow_pyr[iScale].ptr<float>(y)[2*x+1];

            if(point.x <= 0 || point.x >= width || point.y <= 0 || point.y >= height) {
                iTrack = tracks.erase(iTrack);
                continue;
            }

            // get the descriptors for the feature point
            RectInfo rect;
            GetRect(prev_point, rect, width, height, hogInfo);
            GetDesc(hogMat, rect, hogInfo, iTrack->hog, index);
            GetDesc(hofMat, rect, hofInfo, iTrack->hof, index);
            GetDesc(mbhMatX, rect, mbhInfo, iTrack->mbhX, index);
            GetDesc(mbhMatY, rect, mbhInfo, iTrack->mbhY, index);
            iTrack->addPoint(point);

            // draw the trajectories at the first scale
            if(show_track == 1 && iScale == 0)
                DrawTrack(iTrack->point, iTrack->index, fscales[iScale], image);

            // if the trajectory achieves the maximal length
            if(iTrack->index >= trackInfo.length) {
                std::vector<Point2f> trajectory(trackInfo.length+1);
                for(int i = 0; i <= trackInfo.length; ++i)
                    trajectory[i] = iTrack->point[i]*fscales[iScale];
            
                float mean_x(0), mean_y(0), var_x(0), var_y(0), length(0);
                if(IsValid(trajectory, mean_x, mean_y, var_x, var_y, length)) {
                    printf("%f\t%f\t%f\t%f\t%f\t%f\t", mean_x, mean_y, var_x, var_y, length, fscales[iScale]);

                    // output the trajectory
                    for (int i = 0; i < trackInfo.length; ++i)
                        printf("%f\t%f\t", trajectory[i].x,trajectory[i].y);
    
                    PrintDesc(iTrack->hog, hogInfo, trackInfo);
                    PrintDesc(iTrack->hof, hofInfo, trackInfo);
                    PrintDesc(iTrack->mbhX, mbhInfo, trackInfo);
                    PrintDesc(iTrack->mbhY, mbhInfo, trackInfo);
                    printf("\n");
                }

                iTrack = tracks.erase(iTrack);
                continue;
            }
            ++iTrack;
        }
        ReleDescMat(hogMat);
        ReleDescMat(hofMat);
        ReleDescMat(mbhMatX);
        ReleDescMat(mbhMatY);

        if(init_counter != trackInfo.gap)
            continue;

        // detect new feature points every initGap frames
        std::vector<Point2f> points(0);
        for(std::list<Track>::iterator iTrack = tracks.begin(); iTrack != tracks.end(); iTrack++)
            points.push_back(iTrack->point[iTrack->index]);

        DenseSample(grey_pyr[iScale], points, quality, min_distance);
        // save the new feature points
        for(i = 0; i < points.size(); i++)
            tracks.push_back(Track(points[i], trackInfo, hogInfo, hofInfo, mbhInfo));
    }

    if( show_track == 1 ) {
        imshow( "DenseTrack", image);
        c = cv::waitKey(5);
        if((char)c == 27) ;
    }

	if( show_track == 1 )
		destroyWindow("DenseTrack");
}