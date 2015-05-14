#!/usr/bin/env python2

import numpy as np
import cv2
from visual import parser, load_frames, flow2rgb, show
import random
from itertools import product

import sys
def echo(*args, **kwargs):
    end = kwargs.get('end', '\n')

    if len(args) == 1:
        sys.stdout.write(args[0])
    else:
        for arg in args:
            sys.stdout.write(str(arg))
            sys.stdout.write(' ')

    sys.stdout.write(end)


def ssd(image1, center1, image2, center2, size):
    window1 = image1[center1[0] - size : center1[0] + size,
                     center1[1] - size : center1[1] + size]

    window2 = image2[center2[0] - size : center2[0] + size,
                     center2[1] - size : center2[1] + size]

    diff = window1 - window2

    return np.sum(diff ** 2)


SEARCH_FIELD = np.array(((-1, -1), (-1, 0), (-1, 1),
                         ( 0, -1),          ( 0, 1),
                         ( 1, -1), ( 1, 0), ( 1, 1)))


class PatchMatch(object):
    """Dump implementation of the PatchMatch algorithm as described by

    Connelly Barnes, Eli Shechtman, Adam Finkelstein, and Dan B. Goldman.
    PatchMatch: A randomized correspondence algorithm for structural image editing.
    In ACM Transactions on Graphics (Proc. SIGGRAPH), 2009. 2
    """

    def __init__(self, image1, image2, match_radius=5, search_ratio=0.5, search_radius=None, maxoffset=10):
        if not 0 < search_ratio < 1:
            raise ValueError('Search ratio must be in interval (0,1)')

        # input
        self.image1 = image1
        self.image2 = image2
        self.nrows, self.ncols = self.image1.shape

        self.niterations = 0

        # parameters
        self.maxoffset = maxoffset
        self.match_radius = match_radius
        self.search_ratio = search_ratio
        self.search_radius = search_radius or min(image1.shape)

        self.border = self.match_radius + self.maxoffset

        # create an empty matrix with the same x-y dimensions like the first
        # image but with two channels. Each channel stands for an x/y offset
        # of a pixel at this position.
        self.result  = np.zeros(dtype=np.int16, shape=(self.nrows, self.ncols, 2))
        self.quality = np.zeros(dtype=np.float32, shape=(self.nrows, self.ncols))

        # initialize offsets randomly
        self.initialize()

    def __iter__(self):
        rows = xrange(self.border, self.nrows - self.border)
        cols = xrange(self.border, self.ncols - self.border)

        for index in product(rows, cols):
            yield index

    def initialize(self):
        for index in self:
            # create a random offset in 
            offset = random.randint(-self.maxoffset, self.maxoffset), random.randint(-self.maxoffset, self.maxoffset)

            # assing random offset
            self.result[index] = offset

            # calculate the center in the second image by adding the offset
            # to the current index
            center = index[0] + offset[0], index[1] + offset[1]

            self.quality[index] = ssd(self.image1, index, self.image2, center, self.match_radius)

    def iterate(self):
        self.niterations += 1

        # switch between top and left neighbor in even iterations and
        # right bottom neighbor in odd iterations
        neighbor = -1 if self.niterations % 2 == 0 else 1

        for index in self:
            # echo('\r', end='')
            # echo('index', index, end='')
            self.propagate(index, neighbor)
        for index in self:
            self.random_search(index)

    def propagate(self, index, neighbor):
        indices = (index,                           # current position
                   (index[0] + neighbor, index[1]), # top / bottom neighbor
                   (index[0], index[1] + neighbor)) # left / right neighbor
    
        # create an array of all qualities at the above indices
        qualities = np.array((self.quality[indices[0]],
                              self.quality[indices[1]],
                              self.quality[indices[2]]))

        # get the index of the maximal quality
        maxindex = indices[np.argmax(qualities)]

        # get the offset of the neighbor with the maximal quality
        if maxindex != index:
            self.result[index] = self.result[maxindex]

    def random_search(self, index):
        i = 0
        quality = self.quality[index]

        while True:
            distance = int(self.search_radius * self.search_ratio ** i)
            i += 1

            # halt condition. search radius must not be smaller
            # than one pixel
            if distance < 1:
                break

            offset  = distance * random.choice(SEARCH_FIELD)
            center  = index + offset

            if self.border < center[0] < self.nrows - self.border and \
               self.border < center[1] < self.ncols - self.border:
                new_quality = ssd(self.image1, index, self.image2, center, self.match_radius)

                if new_quality > quality:
                    self.result[index] = offset
                    quality = new_quality


def reconstruct_from_flow(flow, image):
    result = np.zeros_like(image)

    for index in np.ndindex(flow.shape[0], flow.shape[1]):
        offset = flow[index]
        pixel  = index[0] + offset[0], index[1] + offset[1]
        result[index] = image[pixel]

    return result


if __name__ == '__main__':
    try:
        # command line parsing
        args = parser.parse_args()
        frame1, frame2 = load_frames(args.image1, args.image2)

        print('initialize ...')
        pm = PatchMatch(frame1, frame2)

        # result after initialziation
        show(reconstruct_from_flow(pm.result, frame2))
        show(flow2rgb(np.float32(pm.result)))

        # do some iterations
        for i in xrange(1):
            print('iteration %d ...' % (i + 1))
            pm.iterate()

            show(reconstruct_from_flow(pm.result, frame2))

            # display final result
            # we have to convert the integer offsets to floats, because
            # optical flow could be subpixel accurate
            rgb = flow2rgb(np.float32(pm.result))
            show(rgb)

    except KeyboardInterrupt:
        print('Stopping ...')
