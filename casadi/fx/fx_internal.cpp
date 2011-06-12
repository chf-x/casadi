/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010 by Joel Andersson, Moritz Diehl, K.U.Leuven. All rights reserved.
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "fx_internal.hpp"
#include "../mx/evaluation.hpp"
#include <typeinfo> 
#include "../stl_vector_tools.hpp"
#include "jacobian.hpp"
#include "mx_function.hpp"

using namespace std;

namespace CasADi{
  
FXInternal::FXInternal(){
  setOption("name",            "unnamed_function"); // name of the function
  addOption("sparse",            OT_BOOLEAN,   true); // function is sparse
  addOption("number_of_fwd_dir", OT_INTEGER,  1); // number of forward derivatives
  addOption("number_of_adj_dir", OT_INTEGER,  1); // number of adjoint derivatives
  addOption("verbose",           OT_BOOLEAN,   false); // verbose evaluation -- for debugging
  addOption("store_jacobians",   OT_BOOLEAN,   false); // keep references to generated Jacobians in order to avoid generating identical Jacobians multiple times
  is_init_ = false;
  verbose_ = false;
}

FXInternal::~FXInternal(){
}

void FXInternal::init(){
  nfdir_ = getOption("number_of_fwd_dir");
  nadir_ = getOption("number_of_adj_dir");
  verbose_ = getOption("verbose");
  store_jacobians_ = getOption("verbose");

  for(vector<FunctionIO>::iterator it=input_.begin(); it!=input_.end(); ++it){
    it->dataF.resize(nfdir_);
    it->dataA.resize(nadir_);
    it->init();
  }

  for(vector<FunctionIO>::iterator it=output_.begin(); it!=output_.end(); ++it){
    it->dataF.resize(nfdir_);
    it->dataA.resize(nadir_);
    it->init();
  }
  
  // Generate storage for generated Jacobians
  if(store_jacobians_){
    jacs_.resize(getNumInputs());
    for(int i=0; i<jacs_.size(); ++i){
      jacs_[i].resize(getNumOutputs());
    }
  }

  // Resize the matrix that holds the sparsity of the Jacobian blocks
  jac_sparsity_.resize(getNumOutputs(),getNumInputs());

  // Mark the function as initialized
  is_init_ = true;
}

void FXInternal::print(ostream &stream) const{
  stream << "function(\"" << getOption("name") << "\")";
}

FX FXInternal::hessian(int iind, int oind){
  stringstream ss;
  ss << "FXInternal::hessian: hessian not defined for class " << typeid(*this).name();
  throw CasadiException(ss.str());
}

bool FXInternal::isInit() const{
  return is_init_;
}

FunctionIO& FXInternal::inputStruct(int i){
  if(i<0 || i>=input_.size()){
    stringstream ss;
    ss << "In function " << getOption("name") << ": input " << i << " not in interval [0," << input_.size() << "]"; 
    throw CasadiException(ss.str());
  }
  return input_.at(i);
}

const FunctionIO& FXInternal::inputStruct(int i) const{
  return const_cast<FXInternal*>(this)->inputStruct(i);
}
  
FunctionIO& FXInternal::outputStruct(int i){
  if(i<0 || i>=output_.size()){
    stringstream ss;
    ss << "In function " << getOption("name") << ": output " << i << " not in interval [0," << output_.size() << "]"; 
    throw CasadiException(ss.str());
  }
  return output_.at(i);
}

const FunctionIO& FXInternal::outputStruct(int i) const{
  return const_cast<FXInternal*>(this)->outputStruct(i);
}

void FXInternal::log(const string& msg) const{
  if(verbose()){
    cout << "CasADi log message: " << msg << endl;
  }
}

void FXInternal::log(const string& fcn, const string& msg) const{
  if(verbose()){
    cout << "CasADi log message: In \"" << fcn << "\" --- " << msg << endl;
  }
}

bool FXInternal::verbose() const{
  return verbose_;
}

bool FXInternal::monitored(const string& mod) const{
  return monitors_.count(mod)>0;
}

Matrix<double>& FXInternal::input(int iind){
  return inputStruct(iind).data;
}
    
const Matrix<double>& FXInternal::input(int iind) const{
  return inputStruct(iind).data;
}

Matrix<double>& FXInternal::output(int oind){
  return outputStruct(oind).data;
}
    
const Matrix<double>& FXInternal::output(int oind) const{
  return outputStruct(oind).data;
}

Matrix<double>& FXInternal::fwdSeed(int iind, int dir){
  return inputStruct(iind).dataF.at(dir);
}
    
const Matrix<double>& FXInternal::fwdSeed(int iind, int dir) const{
  return inputStruct(iind).dataF.at(dir);
}

Matrix<double>& FXInternal::fwdSens(int oind, int dir){
  return outputStruct(oind).dataF.at(dir);
}
    
const Matrix<double>& FXInternal::fwdSens(int oind, int dir) const{
  return outputStruct(oind).dataF.at(dir);
}

Matrix<double>& FXInternal::adjSeed(int oind, int dir){
  return outputStruct(oind).dataA.at(dir);
}
    
const Matrix<double>& FXInternal::adjSeed(int oind, int dir) const{
  return outputStruct(oind).dataA.at(dir);
}

Matrix<double>& FXInternal::adjSens(int iind, int dir){
  return inputStruct(iind).dataA.at(dir);
}
    
const Matrix<double>& FXInternal::adjSens(int iind, int dir) const{
  return inputStruct(iind).dataA.at(dir);
}

void FXInternal::setNumInputs(int num_in){
  return input_.resize(num_in);
}

void FXInternal::setNumOutputs(int num_out){
  return output_.resize(num_out);  
}

int FXInternal::getNumInputs() const{
  return input_.size();
}

int FXInternal::getNumOutputs() const{
  return output_.size();
}

const Dictionary & FXInternal::getStats() const {
  return stats_;
}

GenericType FXInternal::getStat(const string & name) const {
  // Locate the statistic
  Dictionary::const_iterator it = stats_.find(name);

  // Check if found
  if(it == stats_.end()){
    stringstream ss;
    ss << "Statistic: " << name << " has not been set." << endl;
    ss << "Note: statistcs are only set after an evaluate call" << endl;
    throw CasadiException(ss.str());
  }

  return GenericType(it->second);
}

std::vector<MX> FXInternal::symbolicInput() const{
  vector<MX> ret(getNumInputs());
  casadi_assert(isInit());
  for(int i=0; i<ret.size(); ++i){
    stringstream name;
    name << "x_" << i;
    ret[i] = MX(name.str(),input(i).sparsity());
  }
  return ret;
}

FX FXInternal::jacobian(const vector<pair<int,int> >& jblocks){
  // Symbolic input
  vector<MX> j_in = symbolicInput();
  
  // Nondifferentiated function
  FX fcn;
  fcn.assignNode(this);
  vector<MX> fcn_eval = fcn.call(j_in);
  
  // Less overhead if only jacobian is requested
  if(jblocks.size()==1 && jblocks.front().second>=0){
    return Jacobian(fcn,jblocks.front().second,jblocks.front().first);
  }
  
  // Outputs
  vector<MX> j_out;
  j_out.reserve(jblocks.size());
  for(vector<pair<int,int> >::const_iterator it=jblocks.begin(); it!=jblocks.end(); ++it){
    // If variable index is -1, we want nondifferentiated function output
    if(it->second==-1){
      // Nondifferentiated function
      j_out.push_back(fcn_eval[it->first]);
      
    } else {
      // Create jacobian for block
      Jacobian J(fcn,it->second,it->first);
      
      if(!J.isNull()){
        J.init();
      
        // Evaluate symbolically
        j_out.push_back(J.call(j_in).at(0));
      } else {
        j_out.push_back(MX::zeros(output(it->first).numel(),input(it->second).numel()));
      }
    }
  }
  
  // Create function
  return MXFunction(j_in,j_out);
}

CRSSparsity FXInternal::getJacSparsity(int iind, int oind){
  // Dense sparsity by default
  return CRSSparsity(output(oind).numel(),input(iind).numel(),true);
}

CRSSparsity& FXInternal::jacSparsity(int iind, int oind){
  casadi_assert_message(isInit(),"Function not initialized.");
  
  // Get a reference to the block
  CRSSparsity& jsp = jac_sparsity_.elem(oind,iind);
  
  // Generate, if null
  if(jsp.isNull()){
    jsp = getJacSparsity(iind,oind);
  }
  
  // If still null, not dependent
  if(jsp.isNull()){
    jsp = CRSSparsity(output(oind).numel(),input(iind).numel());
  }
  
  // Return a reference to the block
  return jsp;
}



// void setv(double val, vector<double>& v){
//   if(v.size() != 1) throw CasadiException("setv(double,vector<double>&): dimension mismatch");
//   v[0] = val;
// }
// 
// void setv(int val, vector<double>& v){
//   setv(double(val),v);
// }
// 
// void setv(const double* val, vector<double>& v){
//   // no checking
//   copy(val,val+v.size(),v.begin());
// }
// 
// void setv(const vector<double>& val, vector<double>& v){
//   if(v.size() != val.size()) throw CasadiException("setv(const vector<double>&,vector<double>&): dimension mismatch");
//   copy(val.begin(),val.end(),v.begin());
// }
//   
// void getv(double &val, const vector<double>& v){
//   if(v.size() != 1) throw CasadiException("getv(double&,vector<double>&): dimension mismatch");
//   val = v[0];  
// }
// 
// void getv(double* val, const vector<double>& v){
//   // no checking
//   copy(v.begin(),v.end(),val);
// }
// 
// void getv(vector<double>& val, const vector<double>& v){
//   if(v.size() != val.size()) throw CasadiException("getv(vector<double>&,const vector<double>&): dimension mismatch");
//   copy(v.begin(),v.end(),val.begin());
// }

} // namespace CasADi

