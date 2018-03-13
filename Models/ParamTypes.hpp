// Copyright 2018 Google LLC. All Rights Reserved.
/*
  Copyright (C) 2005 Steven L. Scott

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

#ifndef BOOM_PARAM_TYPES_H
#define BOOM_PARAM_TYPES_H

#include "Models/DataTypes.hpp"

namespace BOOM {

  class Params : virtual public Data {
    // abstract base class.  Params inherit from data so that the
    // parameters of one level in a hierarchical model can be viewed as
    // data for the next.

   public:
    //---------- construction, assignment, operator=/== ---------
    Params();
    Params(const Params &rhs);  // does not copy io buffer
    ~Params() override {}
    Params *clone() const override = 0;
    // copied/cloned params have distinct data and distinct io buffers

    //----------------------------------------------------------------------
    // Params can be 'vectorized' which is useful for io and
    // message passing.
    // The "size" of a parameter is the number of elements that it
    // occupies when represented as a vector.
    // Args:
    //   minimal: If true then the size refers to the size of the
    //     smallest vector that can represent the parameter.  If false
    //     then the size is the size of a convenient human readable
    //     representation.  Examples: A symmetric matrix might store
    //     only the upper triangle if minimal is true, and the whole
    //     matrix if it is false.  A probability distribution might
    //     store p-1 numbers instead of p, because the last number is
    //     available because the probabilities sum to 1.
    virtual uint size(bool minimal = true) const = 0;
    virtual Vector vectorize(bool minimal = true) const = 0;

    // Params can be restored from a previously vectorized Vector.
    // This is useful for serializing data (e.g. storing a model to
    // disk) or for communication between machines.
    //
    // It is important that child classes call potential observers
    // when data is restored using this mechanism.  The simplest way
    // to make sure this happens is to call the set() function from
    // the underlying Data class.
    virtual Vector::const_iterator unvectorize(Vector::const_iterator &v,
                                               bool minimal = true) = 0;
    virtual Vector::const_iterator unvectorize(const Vector &v,
                                               bool minimal = true) = 0;
  };

  //============================================================
  //---- non-member functions for vectorizing lots of params ----
  typedef std::vector<Ptr<Params> > ParamVector;

  Vector vectorize(const ParamVector &v, bool minimal = true);
  void unvectorize(ParamVector &pvec, const Vector &v, bool minimal = true);

  ostream &operator<<(ostream &out, const ParamVector &v);

  //============================================================

  class UnivParams : virtual public Params, public DoubleData {
   public:
    UnivParams();
    UnivParams(double x);
    UnivParams(const UnivParams &rhs);
    UnivParams *clone() const override;

    uint size(bool = true) const override { return 1; }
    Vector vectorize(bool minimal = true) const override;
    Vector::const_iterator unvectorize(Vector::const_iterator &v,
                                       bool minimal = true) override;
    Vector::const_iterator unvectorize(const Vector &v,
                                       bool minimal = true) override;
  };

  //------------------------------------------------------------
  class VectorParams : public VectorData, virtual public Params {
   public:
    explicit VectorParams(uint p, double x = 0.0);
    VectorParams(const Vector &v);          // copies v's data
    VectorParams(const VectorParams &rhs);  // copies data
    VectorParams *clone() const override;

    uint size(bool minimal = true) const override;
    Vector vectorize(bool minimal = true) const override;
    Vector::const_iterator unvectorize(Vector::const_iterator &v,
                                       bool minimal = true) override;
    Vector::const_iterator unvectorize(const Vector &v,
                                       bool minimal = true) override;
  };
  //------------------------------------------------------------
  class MatrixParams : public MatrixData, virtual public Params {
   public:
    MatrixParams(uint r, uint c, double x = 0.0);  // zero matrix
    MatrixParams(const Matrix &m);                 // copies m's data
    MatrixParams(const MatrixParams &rhs);         // copies data
    MatrixParams *clone() const override;

    uint size(bool minimal = true) const override;
    Vector vectorize(bool minimal = true) const override;
    Vector::const_iterator unvectorize(Vector::const_iterator &v,
                                       bool minimal = true) override;
    Vector::const_iterator unvectorize(const Vector &v,
                                       bool minimal = true) override;
  };

}  // namespace BOOM
#endif  //  BOOM_PARAM_TYPES_H
